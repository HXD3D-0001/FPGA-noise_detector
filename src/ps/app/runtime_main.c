#include "runtime_main.h"
#include "analysis_service.h"
#include "runtime_control.h"
#include "../drivers/dma_rx_xilinx_freertos.h"
#include "semphr.h"
#include "xparameters.h"
#include "xinterrupt_wrap.h"
#include "xil_io.h"
#include "xiltimer.h"
#include "xtimer_config.h"
#include "../time/timer_scale.h"
#include <string.h>

#if !defined(SDT) || configUSE_TASK_FPU_SUPPORT != 1 || configMAX_PRIORITIES < 8
#error Require genuine modern CPU0 FreeRTOS BSP with registered task FPU context
#endif
#define DATA_OBJECT(name) __attribute__((section(".lfn_data." name),aligned(64),used))
uint8_t lfn_fft_dma[32][65536] DATA_OBJECT("fft_dma");
uint8_t lfn_pcm_dma[128][2048] DATA_OBJECT("pcm_dma");
uint8_t lfn_fft_bd[XAxiDma_BdRingMemCalc(XAXIDMA_BD_MINIMUM_ALIGNMENT,32)] DATA_OBJECT("fft_bd");
uint8_t lfn_pcm_bd[XAxiDma_BdRingMemCalc(XAXIDMA_BD_MINIMUM_ALIGNMENT,128)] DATA_OBJECT("pcm_bd");
BlockPool lfn_packet_pool DATA_OBJECT("packet_pool");
static CaptureTask capture DATA_OBJECT("capture_state");
static LfnAnalysisService analysis DATA_OBJECT("analysis_state");
static XAxiDma dma[2];static DmaRx receive[2];static DmaXilinx backend[2];static DmaXilinxRtos irq_adapter[2];
static TaskHandle_t capture_handle,analysis_handle;
static SemaphoreHandle_t pool_mutex,snapshot_mutex,request_mutex;
static RuntimeControl control;
static struct {uint64_t session;uint32_t config,command;bool pending;} request;
static bool receivers_attempted,receivers_ready;
static XAxiDma_Config *configs[2];
enum { RUNTIME_WAIT_PLATFORM_SOURCE=1,RUNTIME_RINGS_PREPARED=2,RUNTIME_RECOVERY_REQUIRED=3,RUNTIME_STARTUP_FAILED=4 };
volatile struct {
    uint32_t state,capture_heartbeat,analysis_heartbeat,allocation_failures,stack_faults;
    uint32_t dma_fault_mask,prepare_failure,pcm_packets,energy_records,energy_gaps;
    uint32_t control_state,control_failure,request_rejected;
    uint64_t tick_seen,last_pcm_first_raw,last_pcm_end_raw;
    int32_t last_pcm[256];
} lfn_runtime_health;
static volatile bool services_ready;
static bool prepare_attempted;
static bool capture_budget_exhausted;
static uint32_t regs_read(void *ctx,uint32_t offset){(void)ctx;return Xil_In32(XPAR_CONTROL_BASEADDR+offset);}
static int regs_write(void *ctx,uint32_t offset,uint32_t value){(void)ctx;Xil_Out32(XPAR_CONTROL_BASEADDR+offset,value);__asm__ __volatile__("dmb sy":::"memory");return 0;}
static PlRegs regs={NULL,regs_read,regs_write,NULL,1};
static uint64_t clock_us(void *ctx) {
    (void)ctx;XTime ticks;XTime_GetTime(&ticks);
    return lfn_timer_ticks_us(ticks,COUNTS_PER_SECOND);
}
static uint64_t clock_ns(void *ctx){return clock_us(ctx)*1000;}
static bool pool_lock(void *ctx){(void)ctx;return xSemaphoreTake(pool_mutex,0)==pdTRUE;}
static void pool_unlock(void *ctx){(void)ctx;(void)xSemaphoreGive(pool_mutex);}
static void snapshot_lock(void *ctx){(void)ctx;(void)xSemaphoreTake(snapshot_mutex,portMAX_DELAY);}
static void snapshot_unlock(void *ctx){(void)ctx;(void)xSemaphoreGive(snapshot_mutex);}
static void notify_capture(void *ctx){(void)ctx;capture_budget_exhausted=true;xTaskNotifyGive(capture_handle);}
static void packet(void *ctx,const PacketView *p) {
    (void)ctx;
    if(p->header.type==LFN_PCM) {
        for(size_t i=0;i<256;i++){int32_t sample=0;(void)packet_value(p,i,&sample);lfn_runtime_health.last_pcm[i]=sample;}
        lfn_runtime_health.last_pcm_first_raw=p->header.first_raw_sample_index;lfn_runtime_health.last_pcm_end_raw=p->end_raw;lfn_runtime_health.pcm_packets++;
    } else xTaskNotifyGive(analysis_handle);
}
static bool energy(void *ctx,PlEnergy *e){(void)ctx;return pl_energy_read(&regs,e)==PL_OK;}
static bool arm_all(DmaRx *r) {
    for(size_t i=0;i<r->count;i++){DmaToken t;if(!dma_rx_prepare(r,i,&t) || !dma_rx_submit(r,t))return false;}
    return true;
}
static bool init_receivers(void *unused) {
    (void)unused;if(receivers_attempted)return false;receivers_attempted=true;
    configs[0]=XAxiDma_LookupConfig(XPAR_DMA_SPECTRUM_BASEADDR);configs[1]=XAxiDma_LookupConfig(XPAR_DMA_PCM_BASEADDR);
    u32 irq_id[2]={XPAR_DMA_SPECTRUM_INTERRUPTS,XPAR_DMA_PCM_INTERRUPTS};
    void *memory[2]={lfn_fft_dma,lfn_pcm_dma},*bd[2]={lfn_fft_bd,lfn_pcm_bd};
    size_t cap[2]={65536,2048},count[2]={32,128},bd_bytes[2]={sizeof(lfn_fft_bd),sizeof(lfn_pcm_bd)};
    extern uintptr_t IntrControllerAddr;
    for(unsigned i=0;i<2;i++) {
        if(!configs[i] || configs[i]->IntrId[0]!=irq_id[i] || configs[i]->IntrId[1]!=UINT16_MAX ||
           configs[i]->IntrParent!=IntrControllerAddr ||
           XAxiDma_CfgInitialize(&dma[i],configs[i])!=XST_SUCCESS){lfn_runtime_health.prepare_failure=1;return false;}
        DmaXilinxPlatform platform={0};platform.context=&irq_adapter[i];platform.notify_from_isr=dma_rx_xilinx_notify_from_isr;
        if(!dma_rx_xilinx_cache_defaults(&platform) || !dma_rx_xilinx_bind(&backend[i],&receive[i],&dma[i],configs[i],memory[i],cap[i],count[i],bd[i],(UINTPTR)bd[i],bd_bytes[i],&platform)){lfn_runtime_health.prepare_failure=2;return false;}
    }
    receivers_ready=true;return true;
}
bool lfn_runtime_prepare_capture(const CaptureConfig *cfg) {
    if(!cfg || cfg->raw_mode || !receivers_ready || prepare_attempted || services_ready || xTaskGetCurrentTaskHandle()!=capture_handle || !xTaskGetTickCount() ||
       regs_read(NULL,0)!=UINT32_C(0x314e464c) || regs_read(NULL,4)!=1 || regs_read(NULL,0x18)!=0 ||
       regs_read(NULL,0xc4)!=PL_MODE_TEST_PATTERN || regs_read(NULL,0xc8)!=cfg->config_id)return false;
    prepare_attempted=true;lfn_runtime_health.state=RUNTIME_STARTUP_FAILED;
    u32 irq_id[2]={XPAR_DMA_SPECTRUM_INTERRUPTS,XPAR_DMA_PCM_INTERRUPTS};
    CaptureHooks hooks={.monotonic_us=clock_us,.notify_task=notify_capture,.on_packet=packet,.energy_read=energy,.pool_lock=pool_lock,.pool_unlock=pool_unlock};
    LfnAnalysisConfig ac={cfg->config_id,1000,0,false,true};
    LfnAnalysisHooks ah={.monotonic_ns=clock_ns,.pool_lock=pool_lock,.pool_unlock=pool_unlock,.snapshot_lock=snapshot_lock,.snapshot_unlock=snapshot_unlock};
    if(!capture_task_init(&capture,&receive[0],&receive[1],&lfn_packet_pool,cfg,&hooks) || !lfn_analysis_init(&analysis,&lfn_packet_pool,cfg->session_id,cfg->capture_id,&ac,&ah)){lfn_runtime_health.prepare_failure=3;return false;}
    for(unsigned i=0;i<2;i++) {
        u8 priority=(u8)((configMAX_API_CALL_INTERRUPT_PRIORITY+1u)<<portPRIORITY_SHIFT);
        XSetPriorityTriggerType(irq_id[i],priority,configs[i]->IntrParent);
        dma_rx_pause_submissions(&receive[i],false);
        if(!dma_rx_xilinx_rtos_connect_sdt(&irq_adapter[i],&backend[i],&receive[i],capture_handle,irq_id[i],configs[i]->IntrParent) || !arm_all(&receive[i])){lfn_runtime_health.prepare_failure=4;return false;}
    }
    __asm__ __volatile__("dmb sy":::"memory");services_ready=true;lfn_runtime_health.state=RUNTIME_RINGS_PREPARED;return true;
}
static bool prepare_capture(void *ctx,const CaptureConfig *cfg){(void)ctx;return lfn_runtime_prepare_capture(cfg);}
static bool halt_flow(void *ctx,unsigned i){(void)ctx;return dma_rx_xilinx_halt(&backend[i]);}
static bool idle_flow(void *ctx,unsigned i){(void)ctx;return dma_rx_xilinx_idle(&backend[i]);}
static bool reset_flow(void *ctx,unsigned i){(void)ctx;dma_rx_pause_submissions(&receive[i],true);return dma_rx_reset_begin(&receive[i]);}
static bool reset_complete(void *ctx,unsigned i){(void)ctx;return dma_rx_reset_poll(&receive[i]);}
static bool rearm_flow(void *ctx,unsigned i){
    (void)ctx;dma_rx_pause_submissions(&receive[i],false);
    return arm_all(&receive[i]) && dma_rx_xilinx_enable_irq(&backend[i],irq_adapter[i].connected);
}
static void service_capture(void *ctx) {
    (void)ctx;
    if(!services_ready)return;
    for(unsigned i=0;i<2;i++)if(!receive[i].resetting && dma_rx_xilinx_recovery_required(&backend[i])) {
        receive[i].fault=true;lfn_runtime_health.dma_fault_mask|=1u<<i;lfn_runtime_health.state=RUNTIME_RECOVERY_REQUIRED;
    }
    (void)capture_task_run(&capture);
    CaptureEnergyMessage m;
    for(unsigned i=0;i<32 && capture_energy_take(&capture,&m);i++) {
        if(m.gap)lfn_runtime_health.energy_gaps++;else lfn_runtime_health.energy_records++;
    }
}
static uint32_t fault_mask(void *ctx) {
    (void)ctx;uint32_t mask=0;
    for(unsigned i=0;i<2;i++)if(!receive[i].resetting &&
        (receive[i].fault || dma_rx_xilinx_recovery_required(&backend[i])))mask|=1u<<i;
    return mask;
}
static bool enqueue_request(uint64_t session,uint32_t config,uint32_t command) {
    if(!request_mutex || !capture_handle || !xTaskGetTickCount() || xSemaphoreTake(request_mutex,0)!=pdTRUE)return false;
    bool accepted=!request.pending;
    if(accepted){request.session=session;request.config=config;request.command=command;request.pending=true;}
    (void)xSemaphoreGive(request_mutex);if(accepted)xTaskNotifyGive(capture_handle);return accepted;
}
bool lfn_runtime_request_test(uint64_t session,uint32_t config){return session && config && enqueue_request(session,config,0);}
bool lfn_runtime_request_command(uint32_t command){return (command==PL_STOP||command==PL_RESET) && enqueue_request(0,0,command);}
static void capture_entry(void *unused) {
    (void)unused;portTASK_USES_FLOATING_POINT();
    for(;;) {
        (void)ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(10));capture_budget_exhausted=false;
        lfn_runtime_health.capture_heartbeat++;lfn_runtime_health.tick_seen=xTaskGetTickCount();
        if(xSemaphoreTake(request_mutex,0)==pdTRUE){
            if(request.pending){
                bool accepted=request.command?runtime_control_request_command(&control,request.command):runtime_control_request_test(&control,request.session,request.config);
                if(!accepted)lfn_runtime_health.request_rejected++;
                request.pending=false;
            }
            (void)xSemaphoreGive(request_mutex);
        }
        runtime_control_step(&control);lfn_runtime_health.control_state=(uint32_t)control.state;lfn_runtime_health.control_failure=(uint32_t)control.failure;
        if(control.state==RC_FAILED)lfn_runtime_health.state=RUNTIME_RECOVERY_REQUIRED;
        /* Self-notification never monopolizes CPU6; yield at least one tick
         * under backlog so analysis/timer/network tasks can make progress. */
        if(capture_budget_exhausted)vTaskDelay(1);
    }
}
static void analysis_entry(void *unused) {
    (void)unused;portTASK_USES_FLOATING_POINT();
    for(;;) {
        (void)ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(20));lfn_runtime_health.analysis_heartbeat++;
        if(services_ready) {
            (void)lfn_analysis_step(&analysis);
            if(pool_lock(NULL)){bool pending=lfn_packet_pool.count!=0 || analysis.pending_release;pool_unlock(NULL);if(pending)xTaskNotifyGive(analysis_handle);}
        }
        vTaskDelay(1);
    }
}
void vApplicationMallocFailedHook(void){lfn_runtime_health.allocation_failures++;taskDISABLE_INTERRUPTS();for(;;){}}
void vApplicationStackOverflowHook(TaskHandle_t task,char *name){(void)task;(void)name;lfn_runtime_health.stack_faults++;taskDISABLE_INTERRUPTS();for(;;){}}
int main(void) {
    memset((void*)&lfn_runtime_health,0,sizeof(lfn_runtime_health));block_pool_init(&lfn_packet_pool);
    pool_mutex=xSemaphoreCreateMutex();snapshot_mutex=xSemaphoreCreateMutex();request_mutex=xSemaphoreCreateMutex();
    RuntimeControlHooks ch={NULL,clock_us,init_receivers,prepare_capture,service_capture,halt_flow,idle_flow,reset_flow,reset_complete,rearm_flow,fault_mask};
    if(!pool_mutex || !snapshot_mutex || !request_mutex || !runtime_control_init(&control,&regs,&ch)){lfn_runtime_health.state=RUNTIME_STARTUP_FAILED;return 1;}
    lfn_runtime_health.state=RUNTIME_WAIT_PLATFORM_SOURCE;
    if(xTaskCreate(capture_entry,"capture",2048,NULL,6,&capture_handle)!=pdPASS ||
       xTaskCreate(analysis_entry,"analysis",4096,NULL,4,&analysis_handle)!=pdPASS){lfn_runtime_health.state=RUNTIME_STARTUP_FAILED;return 1;}
    /* No ps7_init, producer START, download or fabricated runtime PASS. */
    vTaskStartScheduler();lfn_runtime_health.state=RUNTIME_STARTUP_FAILED;return 1;
}
