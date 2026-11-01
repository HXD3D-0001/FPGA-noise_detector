#include "capture_image.h"
#include "capture_uart_start.h"
#include "../drivers/dma_rx_xilinx_freertos.h"
#include "capture_map.h"
#include "marker.h"
#include "noise_report.h"
#ifdef LFN_STORAGE_RUNTIME
#include "storage_runtime.h"
#include "stored_report_io.h"
#endif
#ifdef LFN_NETWORK
#include "../net/network.h"
#include "network_snapshot.h"
#include "network_capture.h"
#include "network_extra.h"
#endif
#ifdef LFN_EVENT_FIXTURE
#include "event_fixture_image.h"
#endif
#include "semphr.h"
#include "xparameters.h"
#include "xinterrupt_wrap.h"
#include "xil_io.h"
#include "xiltimer.h"
#include "sleep.h"
#include "xtimer_config.h"
#include "../time/timer_scale.h"
#if defined(LFN_SD_PROBE)||defined(LFN_SD_REC)
#include "sd_probe.h"
#endif
#ifdef LFN_SD_REC
#include "sd_rec.h"
#endif
#include "xuartps.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#if !defined(SDT) || configUSE_TASK_FPU_SUPPORT != 1 || configMAX_PRIORITIES < 8
#error Require matching genuine CPU0 FreeRTOS BSP and registered FPU task context
#endif
_Static_assert(LFN_CAPTURE_DMA_SPECTRUM_BASE==XPAR_DMA_SPECTRUM_BASEADDR,"Wrong spectrum DMA BSP");
_Static_assert(LFN_CAPTURE_DMA_PCM_BASE==XPAR_DMA_PCM_BASEADDR,"Wrong PCM DMA BSP");
_Static_assert(LFN_CAPTURE_DMA_SPECTRUM_SDT_ENCODED==XPAR_DMA_SPECTRUM_INTERRUPTS,"Wrong spectrum IRQ BSP");
_Static_assert(LFN_CAPTURE_DMA_PCM_SDT_ENCODED==XPAR_DMA_PCM_INTERRUPTS,"Wrong PCM IRQ BSP");
#define DATA_OBJECT(name) __attribute__((section(".lfn_data." name),aligned(64),used))
uint8_t lfn_fft_dma[32][65536] DATA_OBJECT("fft_dma");
uint8_t lfn_pcm_dma[128][2048] DATA_OBJECT("pcm_dma");
uint8_t lfn_fft_bd[XAxiDma_BdRingMemCalc(XAXIDMA_BD_MINIMUM_ALIGNMENT,32)] DATA_OBJECT("fft_bd");
uint8_t lfn_pcm_bd[XAxiDma_BdRingMemCalc(XAXIDMA_BD_MINIMUM_ALIGNMENT,128)] DATA_OBJECT("pcm_bd");
BlockPool lfn_packet_pool DATA_OBJECT("packet_pool");
static CaptureApplication application DATA_OBJECT("capture_application");
static LfnMarkerStore resident_markers;
static char pending_marker_note[LFN_MARKER_NOTE_BYTES];
static char resident_report[98304] DATA_OBJECT("resident_report");
#ifdef LFN_STORAGE_RUNTIME
static LfnSavedReport saved_reports[2] DATA_OBJECT("saved_reports");
static LfnCompareResult saved_comparison;
#endif
static CaptureControl control;
static LfnServiceBridge services DATA_OBJECT("services");
uint8_t lfn_raw_buffer[LFN_RAW_BUFFER_BYTES] DATA_OBJECT("raw_buffer");
static LfnRawCapture raw_capture;static SemaphoreHandle_t raw_mutex;
static SemaphoreHandle_t service_queue_mutex,service_snapshot_mutex;
#ifdef LFN_STORAGE_RUNTIME
static int storage_restart_rc=-1;
#endif
static bool pre_adc_begun;
static XAxiDma dma[2];static DmaRx receive[2];static DmaXilinx backend[2];static DmaXilinxRtos irq_adapter[2];
volatile struct {uint32_t application_state,runtime_state,failure,request_rejected,bind_failure,malloc_faults,stack_faults;
 uint64_t heartbeat,packets[3],energy_records,energy_gaps;
 uint32_t storage_attached,storage_input_closed,storage_close_requested,storage_detach_rc,control_state,request_state,pre_begun,pre_state;} lfn_capture_image_health;
static XUartPs lfn_uart;static bool lfn_uart_ready;
static TaskHandle_t uart_command_handle;
static QueueHandle_t uart_diag_queue;
volatile uint32_t lfn_uart_tx_timeouts,lfn_uart_diag_drops;
#ifdef LFN_STORAGE_RUNTIME
/* The 100 Hz RTOS poll interval exceeds the 64-byte RX FIFO time at 115200.
 * Drain RX in the ISR; uartcmd remains the only command/TX owner. */
static u8 uart_rx_bytes[1024];
static atomic_uint uart_rx_head,uart_rx_tail,uart_rx_errors;
static void uart_rx_isr(void *unused){
 (void)unused;UINTPTR base=lfn_uart.Config.BaseAddress;
 /* BSP Send enables TXEMPTY whenever RX IRQ is enabled; TX stays polled. */
 XUartPs_WriteReg(base,XUARTPS_IDR_OFFSET,XUARTPS_IXR_TXEMPTY|XUARTPS_IXR_TXFULL);
 u32 status=XUartPs_ReadReg(base,XUARTPS_ISR_OFFSET);
 XUartPs_WriteReg(base,XUARTPS_ISR_OFFSET,status);
 if(status&(XUARTPS_IXR_OVER|XUARTPS_IXR_FRAMING|XUARTPS_IXR_PARITY))atomic_fetch_add(&uart_rx_errors,1);
 while(XUartPs_IsReceiveData(base)){
  u8 b=(u8)XUartPs_ReadReg(base,XUARTPS_FIFO_OFFSET);
  unsigned head=atomic_load_explicit(&uart_rx_head,memory_order_relaxed);
  unsigned next=(head+1u)&1023u;
  if(next==atomic_load_explicit(&uart_rx_tail,memory_order_acquire)){atomic_fetch_add(&uart_rx_errors,1);continue;}
  uart_rx_bytes[head]=b;atomic_store_explicit(&uart_rx_head,next,memory_order_release);
 }
}
static bool uart_rx_start(void){
 if(!lfn_uart_ready||xPortInstallInterruptHandler((uint16_t)XPAR_XUARTPS_0_INTERRUPTS,uart_rx_isr,NULL)!=pdPASS)return false;
 XUartPs_SetFifoThreshold(&lfn_uart,1);
 XUartPs_WriteReg(lfn_uart.Config.BaseAddress,XUARTPS_ISR_OFFSET,XUARTPS_IXR_MASK);
 XUartPs_SetInterruptMask(&lfn_uart,XUARTPS_IXR_RXOVR|XUARTPS_IXR_OVER|XUARTPS_IXR_FRAMING|XUARTPS_IXR_PARITY);
 vPortEnableInterrupt((uint16_t)XPAR_XUARTPS_0_INTERRUPTS);return true;
}
static bool uart_rx_get(u8 *b){
 unsigned tail=atomic_load_explicit(&uart_rx_tail,memory_order_relaxed);
 if(tail==atomic_load_explicit(&uart_rx_head,memory_order_acquire))return false;
 *b=uart_rx_bytes[tail];atomic_store_explicit(&uart_rx_tail,(tail+1u)&1023u,memory_order_release);return true;
}
#endif
static void uart_reply(const char*fmt,...);
/* P2 UART leg: the single BSP UART instance is PS UART1 (base 0xE0001000,
 * carrier MIO12/13). Polled bounded heartbeat only; never blocks capture. */
static void uart_init(void){
 XUartPs_Config*c=XUartPs_LookupConfig(XPAR_XUARTPS_0_BASEADDR);
 if(!c||c->BaseAddress!=XPAR_XUARTPS_0_BASEADDR)return;
 if(XUartPs_CfgInitialize(&lfn_uart,c,XPAR_XUARTPS_0_BASEADDR)!=XST_SUCCESS)return;
 if(XUartPs_SetBaudRate(&lfn_uart,115200)!=XST_SUCCESS)return;
 lfn_uart_ready=true;
 const char banner[]="LFN pre_adc UART1 ready\r\n";
 (void)XUartPs_Send(&lfn_uart,(u8*)banner,(u16)(sizeof banner-1));
}
static void uart_heartbeat(void){
 if(!lfn_uart_ready)return;
 uart_reply("LFN hb=%llu app=%lu rt=%lu fail=%ld rej=%lu\r\n",
  (unsigned long long)lfn_capture_image_health.heartbeat,
  (unsigned long)lfn_capture_image_health.application_state,(unsigned long)lfn_capture_image_health.runtime_state,
  (long)(int32_t)lfn_capture_image_health.failure,(unsigned long)lfn_capture_image_health.request_rejected);
}
/* P2 UART command leg: line protocol BEGIN/START/STOP/STATUS. Commands are
 * marshalled through queues to the capture task, which is the single
 * serialized owner of the pre-ADC controller (begin/command/status/diag). */
typedef struct {uint8_t op;uint64_t token;uint32_t config_id,mode,channel,pattern,source;uint64_t raw_duration,raw_capacity;} UartCommand;
enum {UART_OP_BEGIN=1,UART_OP_START,UART_OP_STOP,UART_OP_STATUS,UART_OP_MARK,UART_OP_REPORT};
typedef struct {uint8_t op;int32_t result;uint64_t id;PreAdcStatus st;int32_t source;uint32_t channel;} UartReply;
static QueueHandle_t uart_cmd_queue,uart_reply_queue,uart_evt_queue;
static uint64_t uart_cmd_seq,uart_session;
#ifdef LFN_NETWORK
static NetworkCapture network_owner,network_published;
static StorageRuntimeHealth network_storage_scratch;
static uint64_t network_start_token;
static bool network_start_pending,network_stop_pending;
static void network_capture_owner_step(void);
static void network_capture_owner_init(void);
static uint64_t now(void *);
static bool network_capture_busy(void){
 bool busy;taskENTER_CRITICAL();
 busy=network_start_pending||(network_published.phase!=NET_CAPTURE_IDLE&&network_published.phase!=NET_CAPTURE_STOPPED);
 taskEXIT_CRITICAL();return busy;
}
static int network_image_control(const char*method,const char*path,const char*body,size_t bytes,char*out,size_t cap){
 bool get=!strcmp(method,"GET"),start=!strncmp(path,"/api/capture/start/",19),stop=!strcmp(path,"/api/capture/stop");
 if(strcmp(path,"/api/capture")&&!start&&!stop)return lfn_network_extra_request(method,path,body,bytes,out,cap);
 if((get&&(start||stop))||(!get&&!start&&!stop))return 405;
 if(bytes)return 400;
 uint64_t token=0;
 if(start){
  const char*p=path+19;if(!*p)return 400;
  for(;*p;p++){if(*p<'0'||*p>'9'||token>(UINT64_MAX-(unsigned)(*p-'0'))/10)return 400;token=token*10+(unsigned)(*p-'0');}
  if(!token)return 400;
 }
 NetworkCapture snapshot;int status=200;
 taskENTER_CRITICAL();
 snapshot=network_published;
 if(start){
  status=network_capture_start(&snapshot,token,now(NULL));
  if(status==202){network_start_token=token;network_start_pending=true;network_published=snapshot;}
 }else if(stop){
  status=network_capture_stop(&snapshot,now(NULL));
  if(status==202){network_stop_pending=true;network_published=snapshot;}
 }
 taskEXIT_CRITICAL();
 if(!network_capture_json(&snapshot,out,cap))return 503;
 return status;
}
#endif
/* Only uartcmd owns the polled transmitter. Send reports the bytes actually
 * accepted by its FIFO; retry the remainder without blocking capture. */
static void uart_write(const char*b,size_t n){
 TickType_t start=xTaskGetTickCount();size_t sent=0;
 while(sent<n){
  sent+=XUartPs_Send(&lfn_uart,(u8*)b+sent,(u32)(n-sent));
  if(sent<n){
   if(xTaskGetTickCount()-start>=pdMS_TO_TICKS(100)){lfn_uart_tx_timeouts++;return;}
   vTaskDelay(1);
  }
 }
}
static bool uart_write_bounded(const char*b,size_t n,TickType_t limit){
 TickType_t start=xTaskGetTickCount();size_t sent=0;
 while(sent<n){
  sent+=XUartPs_Send(&lfn_uart,(u8*)b+sent,(u32)(n-sent));
  if(sent<n){
   if(xTaskGetTickCount()-start>=limit){lfn_uart_tx_timeouts++;return false;}
   vTaskDelay(1);
  }
 }
 return true;
}
static void uart_reply(const char*fmt,...){
 if(!lfn_uart_ready)return;
 char b[128];va_list ap;va_start(ap,fmt);
 int n=vsnprintf(b,sizeof b,fmt,ap);va_end(ap);
 if(n<=0||n>=(int)sizeof b)return;
 if(xTaskGetCurrentTaskHandle()==uart_command_handle)uart_write(b,(size_t)n);
 else if(!uart_diag_queue||xQueueSend(uart_diag_queue,b,0)!=pdTRUE)lfn_uart_diag_drops++;
}
static const char*uart_result_name(int32_t r){
 switch(r){case PRE_ADC_OK:return "OK";case PRE_ADC_DUPLICATE:return "ERR DUP";case PRE_ADC_BUSY:return "ERR BUSY";
 case PRE_ADC_INVALID:return "ERR INVALID";case PRE_ADC_STALE:return "ERR STALE";case PRE_ADC_UNSUPPORTED:return "ERR UNSUP";}
 return "ERR";
}
#ifdef LFN_NETWORK
bool lfn_capture_image_markers_copy(LfnMarkerStore *out){
 if(!out)return false;
 /* CPU0 capture owns updates and never yields while mutating the store. */
 taskENTER_CRITICAL();
 out->count=resident_markers.count;out->next_id=resident_markers.next_id;out->rejected=resident_markers.rejected;
 memcpy(out->items,resident_markers.items,out->count*sizeof(out->items[0]));
 taskEXIT_CRITICAL();return true;
}
static void network_log_line(const char *line){uart_reply("%s\r\n",line);}
static void lfn_image_network_start(void){
 const lfn_network_config_t config={
  .gem_base=0xE000B000u,.ip="169.254.70.21",.mask="255.255.0.0",.gateway="0.0.0.0",
  .mac={0x02,0x70,0x21,0x00,0x00,0x01},.dhcp=true,
  .json=lfn_network_image_json,.asset=NULL,.log=network_log_line,.control=network_image_control};
 if(!lfn_network_start(&config))uart_reply("LFN ERR NET_START\r\n");
}
#endif
/* Capture-task-only view of the latest coherent analysis snapshot.
 * The snapshot is several KiB; keep it in .bss instead of the 16 KiB task stack
 * (a stack-local copy previously tripped the overflow hook). */
static LfnAnalysisSnapshot observation_scratch;
static LfnLevelSet observation_live_scratch;
static bool observation_snapshot(void){
 if(!lfn_analysis_snapshot_copy(&application.analysis,&observation_scratch))return false;
 const LfnSpectrum *sp=&observation_scratch.spectrum;
 return observation_scratch.has_spectrum&&sp->valid&&sp->session_id&&sp->capture_id&&sp->config_id;
}
static bool observation_stamp(LfnSampleStamp *st){
 const CaptureRuntime *rt=&application.runtime;
 const PlCaptureSnapshot *cap=&rt->observed_capture;
 if(!st||rt->state!=CR_RUN||!rt->have_observed_capture||!(cap->flags&PL_CAPTURE_F_POSITION_VALID))return false;
 *st=(LfnSampleStamp){rt->session_id,cap->capture_id,cap->latest_raw,cap->active_config_id,false};
 return true;
}
static bool observation_live_from(const LfnSpectrum *sp,LfnLevelSet *set){
 if(!sp||!set)return false;
 memset(set,0,sizeof(*set));
 set->key.config_hash=sp->config_id;
 set->band_count=LFN_SPECTRUM_BANDS;
 for(unsigned i=0;i<set->band_count;i++){
  const LfnSpectrumBand *b=&sp->bands[i];
  set->bands[i].low_hz=b->low_hz;set->bands[i].high_hz=b->high_hz;
  set->bands[i].power_linear=b->power;
  set->bands[i].below_floor=b->level.status!=LFN_LEVEL_VALID;
 }
 set->duration_ns=sp->observation_seconds>0?(uint64_t)(sp->observation_seconds*1e9):0;
 set->valid_ns=set->duration_ns;set->expected_ns=set->duration_ns;
 return true;
}
static LfnMarkerResult resident_marker_add(uint32_t strength,const char *note,size_t bytes,uint64_t *id){
 if(id)*id=0;
 LfnSampleStamp st;if(!observation_stamp(&st))return LFN_MARKER_INVALID;
 LfnMarkerInput in={0};
 XTime clock;XTime_GetTime(&clock);
 in.start=st;in.strength=strength;in.submit_monotonic_ns=lfn_timer_ticks_us(clock,COUNTS_PER_SECOND)*1000;
 in.note=note;in.note_bytes=bytes;
 LfnMarkerResult r=lfn_marker_add(&resident_markers,&in);
 if(r==LFN_MARKER_ADDED){
  const LfnMarker *m=&resident_markers.items[resident_markers.count-1];
#ifdef LFN_STORAGE_RUNTIME
  if(!lfn_storage_runtime_marker(m)){resident_markers.count--;resident_markers.next_id--;return LFN_MARKER_FULL;}
#endif
  if(id)*id=m->marker_id;
 }
 return r;
}
static bool resident_report_render(size_t *bytes){
 if(bytes) *bytes=0;
 if(!observation_snapshot())return false;
 const LfnSpectrum *sp=&observation_scratch.spectrum;
 if(!observation_live_from(sp,&observation_live_scratch))return false;
 LfnReportData d={0};
 d.session_id=sp->session_id;d.capture_id=sp->capture_id;d.config_id=sp->config_id;
 d.source=sp->test_source?LFN_REPORT_SOURCE_TEST:LFN_REPORT_SOURCE_ADC;
 const PlContractSnapshot *active=&application.runtime.observed_active;
 d.adc_left=application.runtime.have_observed_active&&active->source==PL_CAPTURE_ADC&&active->channel==0&&
   active->plan.session_id==sp->session_id&&active->plan.capture_id==sp->capture_id&&active->plan.config_id==sp->config_id;
 d.has_start_utc=false;
 d.duration_ns=observation_live_scratch.duration_ns;
 d.valid_ns=observation_live_scratch.valid_ns;
 d.expected_ns=observation_live_scratch.expected_ns;
 d.markers=resident_markers.items;d.marker_count=resident_markers.count;
 d.has_comparison=false;d.comparison=0;d.before=0;d.after=0;d.live=&observation_live_scratch;
 if(!lfn_report_json(&d,resident_report,sizeof resident_report))return false;
 *bytes=strlen(resident_report);return *bytes>0;
}
static void uart_send_and_report(const UartCommand*uc){
 if(!uart_cmd_queue||xQueueSend(uart_cmd_queue,uc,0)!=pdTRUE){uart_reply("LFN ERR QUEUE\r\n");return;}
 UartReply ur;
 if(!uart_reply_queue||xQueueReceive(uart_reply_queue,&ur,pdMS_TO_TICKS(2000))!=pdTRUE){uart_reply("LFN ERR TIMEOUT\r\n");return;}
 if(ur.op==UART_OP_STATUS){
  uart_reply("LFN ST state=%lu start=%lu stop=%lu cap=%llu cfg=%lu fail=%ld raw=%lu source=%s ch=%lu\r\n",
   (unsigned long)ur.st.state,(unsigned long)ur.st.start,(unsigned long)ur.st.stop,
   (unsigned long long)ur.st.capture_id,(unsigned long)ur.st.config_id,(long)ur.st.failure,
   (unsigned long)ur.st.raw_reason,ur.source==PL_CAPTURE_ADC?"ADC":ur.source==PL_CAPTURE_TEST?"TEST":"NONE",(unsigned long)ur.channel);
 }else if(ur.op==UART_OP_BEGIN){
  uart_reply("%s BEGIN\r\n",ur.result==0?"LFN OK":"LFN ERR");
 }else if(ur.op==UART_OP_MARK){
  if(ur.result==LFN_MARKER_ADDED)uart_reply("LFN OK MARK id=%llu state=QUEUED\r\n",(unsigned long long)ur.id);
  else uart_reply("LFN ERR MARK %s\r\n",ur.result==LFN_MARKER_FULL?"FULL":"INVALID");
 }else if(ur.op==UART_OP_REPORT){
  if(ur.result==0&&ur.id){
   uart_reply("LFN OK REPORT bytes=%llu\r\n",(unsigned long long)ur.id);
   (void)uart_write_bounded(resident_report,(size_t)ur.id,pdMS_TO_TICKS(5000));
   (void)uart_write_bounded("\r\n",2,pdMS_TO_TICKS(100));
  }else uart_reply("LFN ERR REPORT\r\n");
 }else{
  uart_reply("LFN %s id=%llu\r\n",uart_result_name(ur.result),(unsigned long long)ur.id);
 }
}
#if defined(LFN_SD_PROBE)||defined(LFN_SD_REC)
static void sd_probe_emit(const char *line){if(line)uart_write(line,strlen(line));}
#endif
#ifdef LFN_SD_PROBE
/* 4144 bytes need ~360ms at 115200 baud. Only this task sends frames. */
static bool sd_export_send(const uint8_t *b,size_t n){
 if(!lfn_uart_ready)return false;
 TickType_t start=xTaskGetTickCount();size_t sent=0;
 while(sent<n){
  sent+=XUartPs_Send(&lfn_uart,(u8*)b+sent,(u32)(n-sent));
  if(sent<n){
   if(xTaskGetTickCount()-start>=pdMS_TO_TICKS(1000)){lfn_uart_tx_timeouts++;return false;}
   vTaskDelay(1);
  }
 }
 return true;
}
static uint32_t sd_export_now(void){return (uint32_t)(xTaskGetTickCount()*portTICK_PERIOD_MS);}
#endif
#ifdef LFN_STORAGE_RUNTIME
static bool report_reader_owner(void *unused){(void)unused;return xTaskGetCurrentTaskHandle()==uart_command_handle;}
static void uart_stored_report(const char *first,const char *second){
 StorageRuntimeHealth h;
 if(!lfn_storage_runtime_health(&h)||h.active||h.failed||(h.session&&!h.closed)){
  uart_reply("LFN ERR REPORT RECORDING_ACTIVE\r\n");return;
 }
 char path[64];
 if(!first||!*first){
  if(!h.session){uart_reply("LFN ERR REPORT PATH_REQUIRED\r\n");return;}
  if(h.epoch)(void)snprintf(path,sizeof(path),"SR%06X/E%06X/%cSUM.LFR",(unsigned)(h.session&0xFFFFFF),(unsigned)h.epoch,(h.epoch&1)?'B':'A');
  else (void)snprintf(path,sizeof(path),"SR%06X/ASUM.LFR",(unsigned)(h.session&0xFFFFFF));
  first=path;
 }
 LfnReportData data;
 if(!lfn_stored_report_read(&saved_reports[0],first,report_reader_owner,NULL)||!lfn_saved_report_finish(&saved_reports[0],&data)){
  uart_reply("LFN ERR REPORT READ_OR_SCHEMA code=%u\r\n",(unsigned)saved_reports[0].error);return;
 }
 if(second){
  if(!lfn_stored_report_read(&saved_reports[1],second,report_reader_owner,NULL)||
     !lfn_saved_report_finish(&saved_reports[1],&data)||
     !lfn_saved_report_compare(&saved_reports[0],&saved_reports[1],&saved_comparison)){
   uart_reply("LFN ERR COMPARE READ_OR_SCHEMA\r\n");return;
  }
  data.has_comparison=true;data.comparison=&saved_comparison;
  data.before=&saved_reports[0].levels;data.after=&saved_reports[1].levels;
 }
 if(!lfn_report_json(&data,resident_report,sizeof(resident_report))){uart_reply("LFN ERR REPORT CAPACITY\r\n");return;}
 size_t length=strlen(resident_report);
 uart_reply("LFN OK %s bytes=%u source=SD\r\n",second?"COMPARE":"REPORT",(unsigned)length);
 (void)uart_write_bounded(resident_report,length,pdMS_TO_TICKS(15000));
 (void)uart_write_bounded("\r\n",2,pdMS_TO_TICKS(100));
}
#endif
#ifdef LFN_NETWORK
/* Network jobs are copied in the HTTP task. Markers execute only in capture;
 * stored reports execute only in uartcmd, the existing read-only SD owner. */
static NetworkExtra network_extra;
static int network_marker_apply(void *unused,unsigned strength,const char *note,size_t bytes,uint64_t *id){
 (void)unused;LfnMarkerResult r=resident_marker_add(strength,note,bytes,id);
 return r==LFN_MARKER_ADDED?0:(int)r+1;
}
static int network_report_read(void *unused,const char *first,const char *second,char *out,size_t capacity){
 (void)unused;StorageRuntimeHealth h;
 if(network_capture_busy()||!lfn_storage_runtime_health(&h)||h.active||h.failed||(h.session&&!h.closed))return 1;
 char path[64];
 if(!*first){
  if(!h.session)return 2;
  if(h.epoch)(void)snprintf(path,sizeof path,"SR%06X/E%06X/%cSUM.LFR",(unsigned)(h.session&0xFFFFFF),(unsigned)h.epoch,(h.epoch&1)?'B':'A');
  else (void)snprintf(path,sizeof path,"SR%06X/ASUM.LFR",(unsigned)(h.session&0xFFFFFF));
  first=path;
 }
 LfnReportData data;
 if(!lfn_stored_report_read(&saved_reports[0],first,report_reader_owner,NULL)||!lfn_saved_report_finish(&saved_reports[0],&data))return 3;
 if(*second){
  if(!lfn_stored_report_read(&saved_reports[1],second,report_reader_owner,NULL)||!lfn_saved_report_finish(&saved_reports[1],&data)||!lfn_saved_report_compare(&saved_reports[0],&saved_reports[1],&saved_comparison))return 4;
  data.has_comparison=true;data.comparison=&saved_comparison;data.before=&saved_reports[0].levels;data.after=&saved_reports[1].levels;
 }
 return lfn_report_json(&data,out,capacity)?0:5;
}
void lfn_image_network_extra_init(void){NetworkExtraHooks hooks={NULL,network_marker_apply,network_report_read};network_extra_init(&network_extra,&hooks);}
int lfn_network_extra_request(const char*m,const char*p,const char*b,size_t n,char*out,size_t cap){return network_extra_request(&network_extra,m,p,b,n,out,cap);}
void lfn_image_network_extra_capture_step(void){network_extra_capture_step(&network_extra);}
void lfn_image_network_extra_io_step(void){network_extra_io_step(&network_extra);}
bool lfn_image_network_extra_busy(void){return network_extra_busy(&network_extra);}
#endif
static void uart_handle_line(char*s){
#ifdef LFN_STORAGE_RUNTIME
 if(!strcmp(s,"REPORT")||!strncmp(s,"REPORT ",7)){
  if(strcmp(s,"REPORT LIVE")){uart_stored_report(s[6]?s+7:NULL,NULL);return;}
 }
 if(!strncmp(s,"COMPARE ",8)){
  char *first=s+8,*second=strchr(first,' ');
  if(!second||second==first||!second[1]||strchr(second+1,' ')){uart_reply("LFN ERR COMPARE SYNTAX\r\n");return;}
  *second++=0;uart_stored_report(first,second);return;
 }
#endif
#ifdef LFN_EVENT_FIXTURE
 if(!strncmp(s,"FXBEGIN ",8)){
  char *end;unsigned long long session=strtoull(s+8,&end,10);
  if(!session||*end||!lfn_event_fixture_image_start((uint64_t)session))uart_reply("LFN ERR FXBEGIN\r\n");
  else uart_reply("LFN OK FXBEGIN session=%llu\r\n",session);
  return;
 }
 if(!strcmp(s,"FXSTAT")){
  EventFixtureImageHealth h;
  if(!lfn_event_fixture_image_health(&h)){uart_reply("LFN ERR FXSTAT\r\n");return;}
  uart_reply("LFN FXSTAT session=%llu started=%d running=%d closed=%d failed=%d stage=%u error=%u\r\n",
   (unsigned long long)h.session,h.started,h.running,h.closed,h.failed,h.stage,h.error);
  uart_reply("LFN FXTIME elapsed_us=%llu samples=%llu events=%llu segments=%llu dir=%s\r\n",
   (unsigned long long)h.elapsed_us,(unsigned long long)h.samples,(unsigned long long)h.events,(unsigned long long)h.segments,h.directory);
  uart_reply("LFN FXDATA pcm=%llu energy=%llu file=%llu mon_synced=%llu event_synced=%llu\r\n",
   (unsigned long long)h.pcm_packets,(unsigned long long)h.energy_windows,(unsigned long long)h.event_file_id,
   (unsigned long long)h.mon_synced,(unsigned long long)h.event_synced);
  uart_reply("LFN FXEND lag_us=%llu roll=%u roll_error=%u factory=%u engine=%u stack_words=%u\r\n",
   (unsigned long long)h.max_source_lag_us,h.roll_state,h.roll_error,h.factory_state,h.engine_state,h.stack_free_words);
  return;
 }
 if(!strncmp(s,"BEGIN ",6)||!strncmp(s,"START ",6)||!strncmp(s,"STARTADC ",9)){uart_reply("LFN ERR FIXTURE_ONLY use FXBEGIN\r\n");return;}
#endif
#ifdef LFN_SD_PROBE
 if(!strncmp(s,"SDXOPEN ",8)&&application.state!=CA_WAIT&&
    !(application.state==CA_ACTIVE&&application.runtime.state==CR_STOPPED)){
  (void)lfn_sd_export_command(uart_command_handle,sd_export_send,"SDXBUSY",sd_export_now());return;
 }
 if(lfn_sd_export_command(uart_command_handle,sd_export_send,s,sd_export_now()))return;
#endif
 UartCommand uc={0};
 if(!strncmp(s,"BEGIN ",6)){
#ifdef LFN_NETWORK
  if(network_capture_busy()||lfn_image_network_extra_busy()){uart_reply("LFN ERR NETWORK_CAPTURE_BUSY\r\n");return;}
#endif
  uc.token=strtoull(s+6,0,10);
  if(!uc.token){uart_reply("LFN ERR TOKEN\r\n");return;}
  uc.op=UART_OP_BEGIN;
#ifdef LFN_STORAGE_RUNTIME
  if(pre_adc_begun){
   bool can=false;for(unsigned k=0;k<300;k++){if(lfn_capture_image_can_rebegin(uc.token)){can=true;break;}vTaskDelay(1);}
   if(!can){uart_reply("LFN ERR SESSION\r\n");return;}
  }
  if(!lfn_storage_runtime_session(uc.token)){uart_reply("LFN ERR STORAGE_SESSION\r\n");return;}
  bool ready=false;
  for(unsigned k=0;k<300;k++){if(lfn_storage_runtime_ready_for_start()){ready=true;break;}vTaskDelay(1);}
  if(!ready){
   StorageRuntimeHealth h;(void)lfn_storage_runtime_health(&h);
   uart_reply("LFN ERR STORAGE_ATTACH active=%d failed=%d state=%u phase=%u error=%u stage=%u steps=%llu\r\n",
    h.active?1:0,h.failed?1:0,h.state,h.phase,h.error,(unsigned)h.sd_stage,(unsigned long long)h.steps);
   return;
  }
#endif
 }else if(!strncmp(s,"START ",6)||!strncmp(s,"STARTADC ",9)){
#ifdef LFN_NETWORK
  if(network_capture_busy()||lfn_image_network_extra_busy()){uart_reply("LFN ERR NETWORK_CAPTURE_BUSY\r\n");return;}
#endif
  CaptureUartStart start;
  if(!capture_uart_parse_start(s,&start)){uart_reply("LFN ERR SYNTAX\r\n");return;}
  uc.op=UART_OP_START;uc.config_id=start.config.config_id;uc.source=start.config.source;
  uc.mode=start.config.mode;uc.channel=start.config.channel;uc.pattern=start.config.pattern;
  uc.raw_duration=start.raw_duration;uc.raw_capacity=start.raw_capacity;
 }else if(!strcmp(s,"STOP")){uc.op=UART_OP_STOP;uc.mode=0;
#ifdef LFN_STORAGE_RUNTIME
  lfn_storage_runtime_stop();
#endif
 }
 else if(!strcmp(s,"STOP D")){uc.op=UART_OP_STOP;uc.mode=1;
#ifdef LFN_STORAGE_RUNTIME
  lfn_storage_runtime_stop();
#endif
 }
 else if(!strcmp(s,"STATUS")){uc.op=UART_OP_STATUS;}
 else if(!strncmp(s,"MARK ",5)){
  char *a=s+5;
  if(a[0]<'1'||a[0]>'5'||(a[1]&&a[1]!=' ')){uart_reply("LFN ERR MARK SYNTAX\r\n");return;}
  char *note=a+1;if(*note==' ')note++;
  size_t n=strlen(note);
  if(n>LFN_MARKER_NOTE_BYTES){uart_reply("LFN ERR MARK NOTE_LONG\r\n");return;}
  memcpy(pending_marker_note,note,n);uc.channel=(uint32_t)(a[0]-'0');
  uc.raw_capacity=n;uc.op=UART_OP_MARK;
 }
 else if(!strcmp(s,"REPORT")||!strcmp(s,"REPORT LIVE")){uc.op=UART_OP_REPORT;}
#ifdef LFN_STORAGE_RUNTIME
 else if(!strcmp(s,"SDROTATE")){
  if(!lfn_storage_runtime_request_rotation())uart_reply("LFN ERR SDROTATE\r\n");
  else uart_reply("LFN OK SDROTATE\r\n");
  return;
 }
 else if(!strncmp(s,"SDAUTO ",7)){
  unsigned long v=0;if(sscanf(s+7,"%lu",&v)!=1||v>1){uart_reply("LFN ERR SDAUTO\r\n");return;}
  (void)lfn_storage_runtime_auto_rotate(v==1);
  uart_reply("LFN OK SDAUTO auto=%lu\r\n",v);return;
 }
 else if(!strcmp(s,"SDSTAT")){
  StorageRuntimeHealth h;(void)lfn_storage_runtime_health(&h);
  uart_reply("LFN SDSTAT started=%d active=%d closed=%d failed=%d session=%llu state=%u phase=%u error=%u stage=%u steps=%llu\r\n",
   h.started?1:0,h.active?1:0,h.closed?1:0,h.failed?1:0,
   (unsigned long long)h.session,h.state,h.phase,h.error,(unsigned)h.sd_stage,(unsigned long long)h.steps);
  uart_reply("LFN SDROT rotations=%u epoch=%u gen=%llu rest=%d rc=%d rot=%u/%u\r\n",h.rotations,h.epoch,(unsigned long long)h.route_generation,h.restart_pending?1:0,storage_restart_rc,h.rotation_stage,h.rotation_error);
  uart_reply("LFN SDAPP att=%u in=%u cr=%u det=%u ctl=%u req=%u pre=%u pst=%u\r\n",
   lfn_capture_image_health.storage_attached,lfn_capture_image_health.storage_input_closed,
   lfn_capture_image_health.storage_close_requested,lfn_capture_image_health.storage_detach_rc,
   lfn_capture_image_health.control_state,lfn_capture_image_health.request_state,
   lfn_capture_image_health.pre_begun,lfn_capture_image_health.pre_state);
  const StorageRuntimeDiagnostics *d=&h.diagnostics;
  uart_reply("LFN SDROUTE valid=%u step=%llu blocked=%u kind=%u error=%u accepted=%llu\r\n",
   d->valid?1u:0u,(unsigned long long)d->step,d->blocked?1u:0u,d->error_kind,d->error,(unsigned long long)d->accepted);
  uart_reply("LFN SDLOSS dropped=%llu overflow=%llu\r\n",(unsigned long long)d->dropped,(unsigned long long)d->overflow);
  uart_reply("LFN SDMARK queued=%llu synced=%llu failed=%llu pending=%u error=%u\r\n",
   (unsigned long long)d->markers.queued,(unsigned long long)d->markers.synced,
   (unsigned long long)d->markers.failed,d->markers.pending,d->markers.error);
  uart_reply("LFN SDMARKREF last=%llu unlinked=%llu\r\n",(unsigned long long)d->markers.last_id,(unsigned long long)d->markers.unlinked);
  for(unsigned i=0;i<STORAGE_OWNER_FILES;i++){
   const StorageRuntimeFileHealth *f=&d->files[i];
   uart_reply("LFN SDFILE i=%u bytes=%llu accepted=%llu synced=%llu\r\n",i,
    (unsigned long long)f->bytes,(unsigned long long)f->accepted,(unsigned long long)f->synced);
   uart_reply("LFN SDERR i=%u fatal=%u/%u invalid=%llu jobs=%u/%u unsaved=%u overflow=%llu\r\n",i,
    f->service_fatal?1u:0u,f->pump_fatal?1u:0u,(unsigned long long)f->invalid,
    f->queue_valid?1u:0u,f->live_jobs,f->unsaved,(unsigned long long)f->unsaved_overflow);
  }
  uart_reply("LFN SDEND files=%u\r\n",(unsigned)STORAGE_OWNER_FILES);
  return;
 }
#endif
#ifdef LFN_SD_PROBE
 else if(!strncmp(s,"SDVERIFY ",9)){
  unsigned long long session=0,capture=0;unsigned config=0;char path[48]={0};
   if(sscanf(s+9,"%llu %llu %u %47s",&session,&capture,&config,path)!=4||!session||!capture||!config){uart_reply("LFN ERR SYNTAX\r\n");return;}
  if(application.state!=CA_WAIT&&!(application.state==CA_ACTIVE&&application.runtime.state==CR_STOPPED)){
   uart_reply("LFN SD WARN busy\r\n");return;
  }
  (void)lfn_sd_verify_run(uart_command_handle,sd_probe_emit,(uint64_t)session,(uint64_t)capture,(uint32_t)config,path);
  return;
 }
 else if(!strcmp(s,"SDPROBE")){
  if(application.state==CA_ACTIVE&&(application.runtime.state==CR_BIND||application.runtime.state==CR_RUN)){uart_reply("LFN SD WARN busy\r\n");return;}
  (void)lfn_sd_probe_run(uart_command_handle,sd_probe_emit);
  return;
 }
 else if(!strcmp(s,"SDDEL")){
  if(application.state==CA_ACTIVE&&(application.runtime.state==CR_BIND||application.runtime.state==CR_RUN)){uart_reply("LFN SD WARN busy\r\n");return;}
  (void)lfn_sd_cleanup_run(uart_command_handle,sd_probe_emit);
  return;
 }
 else if(!strcmp(s,"SDWRITE")){
  if(application.state==CA_ACTIVE&&(application.runtime.state==CR_BIND||application.runtime.state==CR_RUN)){uart_reply("LFN SD WARN busy\r\n");return;}
  (void)lfn_sd_write_probe_run(uart_command_handle,sd_probe_emit);
  return;
 }
#endif
#ifdef LFN_SD_REC
 else if(!strncmp(s,"SDREC",5)){
  if(application.state==CA_ACTIVE&&(application.runtime.state==CR_BIND||application.runtime.state==CR_RUN)){uart_reply("LFN SD WARN busy\r\n");return;}
  (void)lfn_sd_rec_run(sd_probe_emit);
  return;
 }
 else if(!strncmp(s,"SDLOAD ",7)){
  unsigned count=(unsigned)strtoul(s+7,0,10);
  if(application.state==CA_ACTIVE&&(application.runtime.state==CR_BIND||application.runtime.state==CR_RUN)){uart_reply("LFN SD WARN busy\r\n");return;}
  (void)lfn_sd_load_run(count,sd_probe_emit);
  return;
 }
 else if(!strncmp(s,"SDOPEN ",7)){
  unsigned long long sess=0;char name[24]={0};
  if(sscanf(s+7,"%llu %23s",&sess,name)<2){uart_reply("LFN ERR SYNTAX\r\n");return;}
  if(application.state==CA_ACTIVE&&(application.runtime.state==CR_BIND||application.runtime.state==CR_RUN)){uart_reply("LFN SD WARN busy\r\n");return;}
  (void)lfn_sd_open_run((uint64_t)sess,name,sd_probe_emit);
  return;
 }
 else if(!strncmp(s,"SDMF",4)){
  char dir[16]="LFNM2";
  if(sscanf(s+4,"%15s",dir)!=1)snprintf(dir,sizeof(dir),"LFNM2");
  if(application.state==CA_ACTIVE&&(application.runtime.state==CR_BIND||application.runtime.state==CR_RUN)){uart_reply("LFN SD WARN busy\r\n");return;}
  (void)lfn_sd_mf_run(dir,sd_probe_emit);
  return;
 }
 else if(!strncmp(s,"SDMW ",5)){
  char dir[16]={0};
  if(sscanf(s+5,"%15s",dir)!=1){uart_reply("LFN ERR SYNTAX\r\n");return;}
  if(application.state==CA_ACTIVE&&(application.runtime.state==CR_BIND||application.runtime.state==CR_RUN)){uart_reply("LFN SD WARN busy\r\n");return;}
  (void)lfn_sd_mw_run(dir,sd_probe_emit);
  return;
 }
 else if(!strncmp(s,"SDPURGE ",8)){
  char d[16]={0};
  if(sscanf(s+8,"%15s",d)!=1){uart_reply("LFN ERR SYNTAX\r\n");return;}
  (void)lfn_sd_purge_dir(d,sd_probe_emit);
  return;
 }
 else if(!strncmp(s,"SDPURGEALL",10)){
  (void)lfn_sd_purge_all(sd_probe_emit);
  return;
 }
 else if(!strncmp(s,"SDLS ",5)){
  char dir[40]={0};
  if(sscanf(s+5,"%39s",dir)!=1){uart_reply("LFN ERR SYNTAX\r\n");return;}
  (void)lfn_sd_ls_run(dir,sd_probe_emit);
  return;
 }
 else if(!strncmp(s,"SDREPLAY ",9)){
  unsigned long long sess=0;char dir[16]={0};
  if(sscanf(s+9,"%llu %15s",&sess,dir)<2){uart_reply("LFN ERR SYNTAX\r\n");return;}
  (void)lfn_sd_replay_run((uint64_t)sess,dir,sd_probe_emit);
  return;
 }
 else if(!strcmp(s,"SDRECX")){
  if(application.state==CA_ACTIVE&&(application.runtime.state==CR_BIND||application.runtime.state==CR_RUN)){uart_reply("LFN SD WARN busy\r\n");return;}
  (void)lfn_sd_rec_run(sd_probe_emit);
  return;
 }
#endif
 else if(!strcmp(s,"HELP")){
  uart_reply("LFN ADC: STARTADC <cfg> <mode 0-2> <ch 0/1> [dur_us cap] | mode 0=MONITOR,1=RAW_CAL,2=RAW_BASELINE\r\n");
#ifdef LFN_SD_PROBE
  uart_reply("LFN CMDS: BEGIN <token> | START <cfg> <mode 0-2> <ch 0/1> <pat 0-3> [dur_us cap] | STOP [D] | STATUS | MARK <1-5> [note] | REPORT | SDPROBE | SDWRITE\r\n");
#else
  uart_reply("LFN CMDS: BEGIN <token> | START <cfg> <mode 0-2> <ch 0/1> <pat 0-3> [dur_us cap] | STOP [D] | STATUS | MARK <1-5> [note] | REPORT\r\n");
#endif
  return;}
 else{uart_reply("LFN ERR SYNTAX\r\n");return;}
 uart_send_and_report(&uc);
}
static void uart_command_task(void*v){
 (void)v;portTASK_USES_FLOATING_POINT();
#ifdef LFN_STORAGE_RUNTIME
 unsigned rx_errors=0;
 if(!uart_rx_start()){uart_reply("LFN ERR UART_RX_INIT\r\n");vTaskDelete(NULL);return;}
#endif
#ifdef LFN_NETWORK
 lfn_image_network_start();
#endif
#ifdef LFN_SD_PROBE
 static char line[208];unsigned len=0;bool discard=false;
#else
 static char line[544];unsigned len=0;bool discard=false;
#endif
 for(;;){
  u8 b;
#ifdef LFN_NETWORK
  lfn_image_network_extra_io_step();
#endif
  /* BSP Recv asserts IsReady; a failed UART init must not trap this task. */
#ifdef LFN_STORAGE_RUNTIME
  while(uart_rx_get(&b)){
   unsigned errors=atomic_load(&uart_rx_errors);
   if(errors!=rx_errors){rx_errors=errors;discard=true;len=0;uart_reply("LFN ERR UART_RX_LOSS\r\n");}
#else
  while(lfn_uart_ready&&XUartPs_Recv(&lfn_uart,&b,1)==1){
#endif
#ifdef LFN_SD_PROBE
   if(b=='\n'||b=='\r'){
    if(discard){(void)lfn_sd_export_command(uart_command_handle,sd_export_send,"SDXINVALID",sd_export_now());}
    else if(len){line[len]=0;uart_handle_line(line);}
    len=0;discard=false;
   }else if(!discard){
    if(b<32||b>126||len>=sizeof line-1){discard=true;len=0;}
    else line[len++]=(char)b;
   }
#else
   if(b=='\n'||b=='\r'){
    if(discard)uart_reply("LFN ERR LINE_TOO_LONG\r\n");
    else if(len){line[len]=0;uart_handle_line(line);}
    len=0;discard=false;
   }else if(!discard){if(len<sizeof line-1)line[len++]=(char)b;else{len=0;discard=true;}}
#endif
  }
#ifdef LFN_SD_PROBE
  lfn_sd_export_poll(sd_export_send,sd_export_now());
  if(lfn_sd_export_busy()){vTaskDelay(2);continue;}
#endif
  if(uart_evt_queue){
   PreAdcDiagEvent ev;uint8_t n=0;
   while(n<4&&xQueueReceive(uart_evt_queue,&ev,0)==pdTRUE){
    uart_reply("LFN EVT cmd=%llu st=%lu res=%lu raw=%lu fail=%ld\r\n",
     (unsigned long long)ev.command_id,(unsigned long)ev.state,(unsigned long)ev.result,
     (unsigned long)ev.raw_reason,(long)ev.failure);
    n++;
   }
  }
  char diag[128];
  if(uart_diag_queue&&xQueueReceive(uart_diag_queue,diag,0)==pdTRUE)uart_write(diag,strlen(diag));
  vTaskDelay(2);
 }
}
static TaskHandle_t capture_handle;static SemaphoreHandle_t request_mutex;static bool bind_attempted;
static CaptureRequest request;
static PreAdcController pre_adc DATA_OBJECT("pre_adc_controller");
static PreAdcDiag pre_adc_diag DATA_OBJECT("pre_adc_diag");
#ifdef LFN_STORAGE_RUNTIME
#endif
static uint32_t mmio_read(void*v,uint32_t offset){__asm__ __volatile__("dmb sy":::"memory");uint32_t n=Xil_In32((UINTPTR)v+offset);__asm__ __volatile__("dmb sy":::"memory");return n;}
static int mmio_write(void*v,uint32_t offset,uint32_t value){Xil_Out32((UINTPTR)v+offset,value);__asm__ __volatile__("dmb sy":::"memory");return 0;}
static uint64_t now(void*v){(void)v;XTime t;XTime_GetTime(&t);return lfn_timer_ticks_us(t,COUNTS_PER_SECOND);}
static void notify(void*v){(void)v;xTaskNotifyGive(capture_handle);}
static bool halt(void*v,unsigned f){(void)v;return f<2&&dma_rx_xilinx_drain_halt(&backend[f]);}
static bool idle(void*v,unsigned f){(void)v;return f<2&&dma_rx_xilinx_idle(&backend[f]);}
static bool boot_dma_idle(void*v){
 (void)v;if(bind_attempted)return false;
 const UINTPTR bases[2]={LFN_CAPTURE_DMA_SPECTRUM_BASE,LFN_CAPTURE_DMA_PCM_BASE};
 for(unsigned i=0;i<2;i++){
  uint32_t cr=mmio_read((void*)bases[i],XAXIDMA_RX_OFFSET+XAXIDMA_CR_OFFSET);
  uint32_t sr=mmio_read((void*)bases[i],XAXIDMA_RX_OFFSET+XAXIDMA_SR_OFFSET);
  if((cr&(XAXIDMA_CR_RUNSTOP_MASK|XAXIDMA_CR_RESET_MASK))||!(sr&XAXIDMA_HALTED_MASK)||
     (sr&(XAXIDMA_ERR_ALL_MASK|XAXIDMA_IRQ_ERROR_MASK)))return false;
 }
 return true;
}
static bool rearm(void*v,unsigned f){
 (void)v;if(f>=2)return false;DmaRx*r=&receive[f];dma_rx_pause_submissions(r,false);
 for(size_t i=0;i<r->count;i++){DmaToken token;if(!dma_rx_prepare(r,i,&token)||!dma_rx_submit(r,token))return false;}
 return dma_rx_xilinx_enable_irq(&backend[f],irq_adapter[f].connected);
}
static uint32_t fault(void*v){
 (void)v;uint32_t mask=0;
 for(unsigned i=0;i<2;i++)if(!receive[i].resetting&&(receive[i].fault||dma_rx_xilinx_recovery_required(&backend[i])))mask|=1u<<i;
#ifdef LFN_STORAGE_RUNTIME
 if(lfn_storage_runtime_failed())mask|=4u;
#endif
 return mask;
}
static bool bind_dma(void*v){
 (void)v;if(bind_attempted||xTaskGetCurrentTaskHandle()!=capture_handle||!xTaskGetTickCount())return false;bind_attempted=true;
 UINTPTR bases[2]={LFN_CAPTURE_DMA_SPECTRUM_BASE,LFN_CAPTURE_DMA_PCM_BASE};
 u32 ids[2]={LFN_CAPTURE_DMA_SPECTRUM_SDT_ENCODED,LFN_CAPTURE_DMA_PCM_SDT_ENCODED};
 void*data[2]={lfn_fft_dma,lfn_pcm_dma};void*bd[2]={lfn_fft_bd,lfn_pcm_bd};
 size_t counts[2]={32,128},sizes[2]={65536,2048},bd_sizes[2]={sizeof(lfn_fft_bd),sizeof(lfn_pcm_bd)};
 extern uintptr_t IntrControllerAddr;
 for(unsigned i=0;i<2;i++){
  XAxiDma_Config*c=XAxiDma_LookupConfig(bases[i]);
  if(!c||c->BaseAddr!=bases[i]||c->IntrId[0]!=ids[i]||c->IntrId[1]!=UINT16_MAX||
     XGet_BaseAddr(c->IntrParent)!=XGet_BaseAddr(IntrControllerAddr)||
     XGet_IntcType(c->IntrParent)!=XGet_IntcType(IntrControllerAddr)||XAxiDma_CfgInitialize(&dma[i],c)!=XST_SUCCESS){lfn_capture_image_health.bind_failure=1;return false;}
  DmaXilinxPlatform p={0};p.context=&irq_adapter[i];p.notify_from_isr=dma_rx_xilinx_notify_from_isr;
  if(!dma_rx_xilinx_cache_defaults(&p)||!dma_rx_xilinx_bind(&backend[i],&receive[i],&dma[i],c,data[i],sizes[i],counts[i],bd[i],(UINTPTR)bd[i],bd_sizes[i],&p)){lfn_capture_image_health.bind_failure=2;return false;}
  u8 priority=(u8)((configMAX_API_CALL_INTERRUPT_PRIORITY+1u)<<portPRIORITY_SHIFT);
  XSetPriorityTriggerType(ids[i],priority,c->IntrParent);
  if(!dma_rx_xilinx_rtos_connect_sdt(&irq_adapter[i],&backend[i],&receive[i],capture_handle,ids[i],c->IntrParent)){lfn_capture_image_health.bind_failure=3;return false;}
 }
 return true;
}
static StorageResult enqueue(CaptureRequestKind kind,uint64_t session,const PlCaptureConfig*c,const CaptureApplicationLimits*l,CaptureStorageRoute*route,bool retain,uint64_t*ticket){
 if(!request_mutex||!capture_handle||!xTaskGetTickCount())return STORE_BUSY;
 CaptureRequestCommand command={0};command.kind=kind;command.session=session;command.route=route;
 if(c)command.config=*c;
 if(l)command.limits=*l;
 StorageResult result=capture_request_submit(&request,&command,retain,ticket);
 if(result==STORE_OK)xTaskNotifyGive(capture_handle);
 return result;
}
bool lfn_capture_image_request_start(uint64_t session,const PlCaptureConfig*c,const CaptureApplicationLimits*l){uint64_t ticket;return session&&c&&l&&enqueue(REQUEST_START,session,c,l,NULL,false,&ticket)==STORE_OK;}
bool lfn_capture_image_request_stop(bool discard){uint64_t ticket;return enqueue(discard?REQUEST_DISCARD:REQUEST_STOP,0,NULL,NULL,NULL,false,&ticket)==STORE_OK;}
bool lfn_capture_image_storage_attach(CaptureStorageRoute*r){uint64_t ticket;return r&&enqueue(REQUEST_ATTACH,0,NULL,NULL,r,false,&ticket)==STORE_OK;}
bool lfn_capture_image_storage_close(void){uint64_t ticket;return enqueue(REQUEST_CLOSE,0,NULL,NULL,NULL,false,&ticket)==STORE_OK;}
StorageResult lfn_capture_image_request_submit_start(uint64_t session,const PlCaptureConfig*c,const CaptureApplicationLimits*l,uint64_t*ticket){
 if(!session||!c||!l||!ticket)return STORE_INVALID;
 return enqueue(REQUEST_START,session,c,l,NULL,true,ticket);
}
StorageResult lfn_capture_image_request_submit_stop(bool discard,uint64_t*ticket){return enqueue(discard?REQUEST_DISCARD:REQUEST_STOP,0,NULL,NULL,NULL,true,ticket);}
StorageResult lfn_capture_image_request_copy(uint64_t t,CaptureRequestReceipt*out){return request_mutex?capture_request_copy(&request,t,out):STORE_BUSY;}
StorageResult lfn_capture_image_request_ack(uint64_t t){return request_mutex?capture_request_ack(&request,t):STORE_BUSY;}
static bool control_lock(void*v){(void)v;return xSemaphoreTake(request_mutex,0)==pdTRUE;}
static void control_unlock(void*v){(void)v;(void)xSemaphoreGive(request_mutex);}
static bool control_owner(void*v){(void)v;return xTaskGetCurrentTaskHandle()==capture_handle;}
static bool control_admit(void*v){(void)v;return request.state==REQUEST_IDLE;}
static bool request_admit(void*v){(void)v;return control.state==CONTROL_IDLE&&!control.barrier.paused;}
StorageResult lfn_capture_image_control_submit(CaptureControlKind k,uint64_t pause,uint64_t generation,uint64_t *ticket){
 if(!request_mutex||!capture_handle||!xTaskGetTickCount())return STORE_BUSY;
 StorageResult result=capture_control_submit(&control,k,pause,generation,ticket);if(result==STORE_OK)xTaskNotifyGive(capture_handle);return result;
}
StorageResult lfn_capture_image_control_attach(CaptureStorageRoute *route,uint64_t *ticket){
 if(!request_mutex||!capture_handle||!xTaskGetTickCount())return STORE_BUSY;
 StorageResult result=capture_control_attach(&control,route,ticket);if(result==STORE_OK)xTaskNotifyGive(capture_handle);return result;
}
StorageResult lfn_capture_image_control_copy(uint64_t t,CaptureControlReceipt *out){return request_mutex?capture_control_copy(&control,t,out):STORE_BUSY;}
StorageResult lfn_capture_image_control_ack(uint64_t t){return request_mutex?capture_control_ack(&control,t):STORE_BUSY;}
bool lfn_capture_image_control_barrier(CaptureControlBarrier *out){return request_mutex&&capture_control_barrier(&control,out);}
static StorageResult pa_submit_start(void*v,uint64_t session,const PlCaptureConfig*c,const CaptureApplicationLimits*l,uint64_t*ticket){(void)v;return lfn_capture_image_request_submit_start(session,c,l,ticket);}
static StorageResult pa_submit_stop(void*v,bool discard,uint64_t*ticket){(void)v;return lfn_capture_image_request_submit_stop(discard,ticket);}
static StorageResult pa_copy(void*v,uint64_t t,CaptureRequestReceipt*out){(void)v;return lfn_capture_image_request_copy(t,out);}
static StorageResult pa_ack(void*v,uint64_t t){(void)v;return lfn_capture_image_request_ack(t);}
static LfnServiceResult pa_snapshot(void*v,LfnServiceSnapshot*out){(void)v;return lfn_capture_image_snapshot_copy(out);}
static const PreAdcHooks pre_adc_hooks={NULL,pa_submit_start,pa_submit_stop,pa_copy,pa_ack,pa_snapshot};
static bool session_teardown_ready(void){
 if(application.storage||application.storage_close_requested||application.storage_input_closed)return false;
 if(application.state!=CA_ACTIVE||application.runtime.state!=CR_STOPPED||application.clock_pending)return false;
 if(control.state!=CONTROL_IDLE||request.state!=REQUEST_IDLE)return false;
#ifdef LFN_STORAGE_RUNTIME
 StorageRuntimeHealth h;
 if(!lfn_storage_runtime_health(&h)||h.active||!h.closed||h.failed||h.restart_pending)return false;
#endif
 return true;
}
bool lfn_capture_image_can_rebegin(uint64_t host_session){
 if(!request_mutex||!host_session)return false;
 if(!pre_adc_begun)return true;
 return pre_adc_controller_can_rebegin(&pre_adc,host_session)&&session_teardown_ready();
}
bool lfn_capture_image_pre_adc_begin(uint64_t host_session){
 if(!request_mutex||!host_session)return false;
 if(!pre_adc_begun){
  if(!pre_adc_controller_init(&pre_adc,host_session,&pre_adc_hooks,&pre_adc_diag))return false;
  pre_adc_begun=true;return true;
 }
 if(!pre_adc_controller_can_rebegin(&pre_adc,host_session))return false;
 if(application.state!=CA_ACTIVE||application.runtime.state!=CR_STOPPED||application.clock_pending)return false;
 if(!pre_adc_controller_rebegin(&pre_adc,host_session))return false;
 return true;
}
PreAdcResult lfn_capture_image_pre_adc_command(const PreAdcCommand*q){return pre_adc_controller_command(&pre_adc,q);}
bool lfn_capture_image_pre_adc_status(PreAdcStatus*s){return pre_adc_controller_status(&pre_adc,s);}
bool lfn_capture_image_pre_adc_diag_take(PreAdcDiagEvent*e){return pre_adc_diag_take(&pre_adc_diag,e);}
#ifdef LFN_NETWORK
static bool network_can_begin(void*v,uint64_t session){(void)v;return !lfn_image_network_extra_busy()&&lfn_capture_image_can_rebegin(session);}
static bool network_storage_begin(void*v,uint64_t session){(void)v;return lfn_storage_runtime_auto_rotate(true)&&lfn_storage_runtime_session(session);}
static bool network_begin(void*v,uint64_t session){
 (void)v;if(!lfn_capture_image_pre_adc_begin(session))return false;
 uart_session=session;lfn_marker_store_init(&resident_markers);return true;
}
static bool network_start_adc(void*v){
 (void)v;PreAdcCommand q={0};q.id=++uart_cmd_seq;q.session_id=uart_session;q.kind=PRE_ADC_START_ADC;
 q.config=(PlCaptureConfig){.config_id=1,.source=PL_CAPTURE_ADC,.mode=0,.channel=0,.pattern=0};
 q.limits.runtime.transaction_us=q.limits.runtime.bind_us=q.limits.runtime.close_us=5000000;
 q.limits.runtime.snapshot_age_ticks=50000;q.limits.owner_us=100000;
 return lfn_capture_image_pre_adc_command(&q)==PRE_ADC_OK;
}
static bool network_stop_adc(void*v){
 (void)v;
 /* Storage acknowledges this intent after any epoch restart has settled. */
 if(!lfn_storage_runtime_prepare_stop())return false;
 lfn_storage_runtime_stop();
 if(!pre_adc_begun)return false;
 if(pre_adc.status.state==PRE_ADC_STOPPED||pre_adc.status.state==PRE_ADC_STOPPING)return true;
 PreAdcCommand q={0};q.id=++uart_cmd_seq;q.session_id=uart_session;q.kind=PRE_ADC_STOP;
 return lfn_capture_image_pre_adc_command(&q)==PRE_ADC_OK;
}
static void network_capture_owner_init(void){
 NetworkCaptureHooks h={NULL,network_can_begin,network_storage_begin,network_begin,network_start_adc,network_stop_adc};
 network_capture_init(&network_owner,&h);network_published=network_owner;
 network_start_token=0;network_start_pending=network_stop_pending=false;
 lfn_image_network_extra_init();
}
static void network_capture_owner_step(void){
 uint64_t token;bool start,stop;taskENTER_CRITICAL();
 token=network_start_token;start=network_start_pending;stop=network_stop_pending;
 network_start_pending=network_stop_pending=false;taskEXIT_CRITICAL();
 uint64_t t=now(NULL);
 if(start)(void)network_capture_start(&network_owner,token,t);
 if(stop)(void)network_capture_stop(&network_owner,t);
 NetworkCaptureObservation o={0};o.have_controller=pre_adc_controller_status(&pre_adc,&o.controller);
 if(o.have_controller&&o.controller.start_command){o.source=pre_adc.start_command.config.source;o.channel=pre_adc.start_command.config.channel;}
 o.storage_available=lfn_storage_runtime_health(&network_storage_scratch);
 if(o.storage_available){o.storage_active=network_storage_scratch.active;o.storage_closed=network_storage_scratch.closed;
  o.storage_failed=network_storage_scratch.failed;o.storage_session=network_storage_scratch.session;}
 o.storage_ready=lfn_storage_runtime_ready_for_start();
 network_capture_step(&network_owner,&o,t);
 taskENTER_CRITICAL();network_published=network_owner;
 if(network_start_pending)(void)network_capture_start(&network_published,network_start_token,t);
 if(network_stop_pending)(void)network_capture_stop(&network_published,t);
 taskEXIT_CRITICAL();
 lfn_image_network_extra_capture_step();
}
#endif
uint32_t lfn_capture_image_status_word(void){
 extern CaptureApplication application;
 return application.cap.read32(application.cap.context,0x0c);
}
static bool service_queue_lock(void*v){(void)v;return xSemaphoreTake(service_queue_mutex,0)==pdTRUE;}
static void service_queue_unlock(void*v){(void)v;(void)xSemaphoreGive(service_queue_mutex);}
static bool service_snapshot_lock(void*v){(void)v;return xSemaphoreTake(service_snapshot_mutex,0)==pdTRUE;}
static void service_snapshot_unlock(void*v){(void)v;(void)xSemaphoreGive(service_snapshot_mutex);}
LfnServiceResult lfn_capture_image_snapshot_copy(LfnServiceSnapshot*out){
 if(!service_snapshot_mutex)return SERVICE_INVALID;
 return lfn_service_snapshot_copy(&services,out);
}
LfnServiceResult lfn_capture_image_energy_take(LfnServiceMessage*out){
 if(!service_queue_mutex)return SERVICE_INVALID;
 return lfn_service_energy_take(&services,out);
}
static bool raw_lock(void*v){(void)v;return xSemaphoreTake(raw_mutex,0)==pdTRUE;}
static void raw_unlock(void*v){(void)v;(void)xSemaphoreGive(raw_mutex);}
LfnRawResult lfn_capture_image_raw_read(uint64_t session,uint64_t capture,uint32_t config,uint64_t ordinal,uint8_t*out,size_t n){
 if(!raw_mutex)return RAW_INVALID;
 return lfn_raw_read(&raw_capture,session,capture,config,ordinal,out,n);
}
static void capture_entry(void*v){
 (void)v;portTASK_USES_FLOATING_POINT();
 lfn_marker_store_init(&resident_markers);
 for(;;){
  (void)ulTaskNotifyTake(pdTRUE,1);
  pre_adc_controller_step(&pre_adc);
#ifdef LFN_NETWORK
  network_capture_owner_step();
#endif
  /* Single-owner marshal: this task alone touches the controller. */
  if(uart_cmd_queue){
   UartCommand uc;
   while(xQueueReceive(uart_cmd_queue,&uc,0)==pdTRUE){
    UartReply ur={0};ur.op=uc.op;ur.result=-1;ur.source=-1;
    if(uc.op==UART_OP_BEGIN){
     ur.result=lfn_capture_image_pre_adc_begin(uc.token)?0:-1;
     if(ur.result==0){uart_session=uc.token;lfn_marker_store_init(&resident_markers);}
    }else if(uc.op==UART_OP_START){
     PreAdcCommand q={0};
     q.id=++uart_cmd_seq;q.session_id=uart_session;q.kind=uc.source==PL_CAPTURE_ADC?PRE_ADC_START_ADC:PRE_ADC_START_TEST;
     q.config.config_id=uc.config_id;q.config.source=uc.source;q.config.mode=uc.mode;
     q.config.channel=uc.channel;q.config.pattern=uc.pattern;
     q.limits.runtime.transaction_us=5000000;q.limits.runtime.bind_us=5000000;
     q.limits.runtime.close_us=5000000;q.limits.runtime.snapshot_age_ticks=50000;
     q.limits.owner_us=100000;
     q.limits.raw.duration_us=uc.raw_duration;q.limits.raw.capacity_bytes=(size_t)uc.raw_capacity;
     ur.result=(int32_t)lfn_capture_image_pre_adc_command(&q);ur.id=q.id;
    }else if(uc.op==UART_OP_STOP){
     PreAdcCommand q={0};
     q.id=++uart_cmd_seq;q.session_id=uart_session;q.kind=PRE_ADC_STOP;q.discard=uc.mode?true:false;
     ur.result=(int32_t)lfn_capture_image_pre_adc_command(&q);ur.id=q.id;
    }else if(uc.op==UART_OP_STATUS){ur.result=0;ur.id=uart_cmd_seq;
    }else if(uc.op==UART_OP_MARK){
     uint64_t id=0;ur.result=(int32_t)resident_marker_add(uc.channel,pending_marker_note,(size_t)uc.raw_capacity,&id);ur.id=id;
    }else if(uc.op==UART_OP_REPORT){
     size_t bytes=0;ur.result=resident_report_render(&bytes)?0:-1;ur.id=(uint64_t)bytes;
    }
    (void)lfn_capture_image_pre_adc_status(&ur.st);
    if(ur.st.start_command){ur.source=(int32_t)pre_adc.start_command.config.source;ur.channel=pre_adc.start_command.config.channel;}
    xQueueOverwrite(uart_reply_queue,&ur);
   }
   PreAdcDiagEvent ev;uint8_t evn=0;
   while(evn<4&&lfn_capture_image_pre_adc_diag_take(&ev)){if(xQueueSend(uart_evt_queue,&ev,0)!=pdTRUE)break;evn++;}
  }
  capture_request_step(&request);
  lfn_capture_image_health.request_rejected=request.rejected>UINT32_MAX?UINT32_MAX:(uint32_t)request.rejected;
#ifdef LFN_STORAGE_RUNTIME
  if(lfn_storage_runtime_capture_close_requested()&&application.storage&&
     application.state==CA_ACTIVE&&application.runtime.state==CR_STOPPED&&
     application.runtime.final_clean&&!application.storage_input_closed&&!application.storage_close_requested)
   (void)capture_application_close_storage(&application);
  lfn_storage_runtime_capture_stopped(!application.clock_pending&&
    ((application.state==CA_WAIT&&!application.bound&&!application.prepared)||
     (application.state==CA_ACTIVE&&application.runtime.state==CR_STOPPED)));
#endif
  capture_control_step(&control);
#ifdef LFN_STORAGE_RUNTIME
  {
   PlCaptureConfig restart_cfg;
   if(lfn_storage_runtime_take_restart(&restart_cfg))
    storage_restart_rc=(int)pre_adc_controller_restart(&pre_adc,&restart_cfg);
  }
  {
   StorageRuntimeHealth h;
   bool torn=lfn_storage_runtime_health(&h)&&!h.active&&h.closed&&!h.failed&&!h.restart_pending;
   if(torn&&application.storage&&application.state==CA_ACTIVE&&
      application.runtime.state==CR_STOPPED){
    StorageResult dr=capture_application_detach_storage(&application);
    lfn_capture_image_health.storage_detach_rc=(uint32_t)(dr+1);
   }
  }
  lfn_capture_image_health.storage_attached=application.storage?1u:0u;
#endif
  lfn_capture_image_health.storage_input_closed=application.storage_input_closed?1u:0u;
  lfn_capture_image_health.storage_close_requested=application.storage_close_requested?1u:0u;
  lfn_capture_image_health.control_state=(uint32_t)control.state;
  lfn_capture_image_health.request_state=(uint32_t)request.state;
  lfn_capture_image_health.pre_begun=pre_adc_begun?1u:0u;
  {PreAdcStatus ps;lfn_capture_image_health.pre_state=pre_adc_controller_status(&pre_adc,&ps)?(uint32_t)ps.state:99u;}
  if(!capture_control_owner_paused(&control))capture_application_step(&application);
  lfn_capture_image_health.heartbeat++;
  /* Loop period is two ticks (notify wait + delay), so tick%N is phase
   * fragile: count iterations instead of sampling the tick counter. */
  static uint32_t uart_div;
  if(++uart_div>=100){uart_div=0;
   bool diag=false;
   /* Keep boot ownership and clock command evidence distinct. */
   if(pre_adc.status.state==PRE_ADC_STARTING||(pre_adc.status.fault_latched&&pre_adc.status.failure==-4)){
    extern CaptureApplication application;
    uint32_t cs=application.cap.read32(application.cap.context,0x0c);
    uint32_t cw=application.cap.read32(application.cap.context,0x08);
    uint32_t cr=application.cap.read32(application.cap.context,0x64);
    uart_reply("LFN CLKDIAG app=%lu st=0x%08lx clk=0x%08lx seq_ack_r=0x%08lx cap_seq=%lu\r\n",
     (unsigned long)application.state,(unsigned long)cs,(unsigned long)cw,(unsigned long)cr,
     (unsigned long)application.cap.read32(application.cap.context,0x10));
    uart_reply("LFN BOOTDIAG st=0x%08lx seq=%lu req=0x%08lx ack=0x%08lx\r\n",
     (unsigned long)application.dat.read32(application.dat.context,0x0c),
     (unsigned long)application.dat.read32(application.dat.context,0x10),
     (unsigned long)application.dat.read32(application.dat.context,0x1c),
     (unsigned long)application.dat.read32(application.dat.context,0x2c));
    diag=true;
   }
   if(!diag)uart_heartbeat();
  }
  lfn_capture_image_health.application_state=(uint32_t)application.state;lfn_capture_image_health.runtime_state=(uint32_t)application.runtime.state;
  lfn_capture_image_health.failure=(uint32_t)(application.failure?application.failure:application.runtime.failure);
  memcpy((void*)lfn_capture_image_health.packets,application.packets,sizeof(application.packets));
  lfn_capture_image_health.energy_records=application.energy_records;lfn_capture_image_health.energy_gaps=application.energy_gaps;
  /* One owner serializes control, capture, energy and one analysis block. */
  vTaskDelay(1);
 }
}
void vApplicationMallocFailedHook(void){lfn_capture_image_health.malloc_faults++;taskDISABLE_INTERRUPTS();for(;;){}}
void vApplicationStackOverflowHook(TaskHandle_t t,char*n){(void)t;(void)n;lfn_capture_image_health.stack_faults++;taskDISABLE_INTERRUPTS();for(;;){}}
int main(void){
 /* The BSP resets the global counter on its first sleep (also used by SD).
  * Establish its epoch before any application deadline or timestamp. */
 usleep(1);
 /* NOLOAD survives JTAG reload: discard the preceding image's callback
  * addresses and events before either task can observe the controller. */
 memset(&pre_adc,0,sizeof(pre_adc));pre_adc_diag_init(&pre_adc_diag);
 memset(&application,0,sizeof(application));block_pool_init(&lfn_packet_pool);
 PlRegs cap={(void*)(UINTPTR)LFN_CAPTURE_CAP_BASE,mmio_read,mmio_write,NULL,1};
 PlRegs dat={(void*)(UINTPTR)LFN_CAPTURE_DAT_BASE,mmio_read,mmio_write,NULL,1};
 PlRegs ctr={(void*)(UINTPTR)LFN_CAPTURE_CTR_BASE,mmio_read,mmio_write,NULL,1};
 CaptureApplicationHooks h={NULL,now,notify,bind_dma,halt,idle,rearm,fault,boot_dma_idle};
 raw_mutex=xSemaphoreCreateMutex();request_mutex=xSemaphoreCreateMutex();service_queue_mutex=xSemaphoreCreateMutex();service_snapshot_mutex=xSemaphoreCreateMutex();
 LfnServiceLocks locks={NULL,service_queue_lock,service_queue_unlock,service_snapshot_lock,service_snapshot_unlock};
 LfnRawLocks raw_locks={NULL,raw_lock,raw_unlock};
 if(!raw_mutex||!lfn_raw_init(&raw_capture,lfn_raw_buffer,sizeof(lfn_raw_buffer),&raw_locks)||!request_mutex||!service_queue_mutex||!service_snapshot_mutex||!lfn_service_bridge_init(&services,&locks)||!capture_application_init(&application,&cap,&dat,&ctr,&receive[0],&receive[1],&lfn_packet_pool,&h)||!capture_application_attach_services(&application,&services)||!capture_application_attach_raw(&application,&raw_capture))return 1;
 StorageQueueLocks control_locks={NULL,control_lock,control_unlock};
 if(!capture_control_init(&control,&application,&control_locks,1,control_owner,control_admit,NULL))return 1;
 if(!capture_request_init(&request,&application,&control_locks,1,control_owner,request_admit,NULL))return 1;
 uart_init();
 uart_cmd_queue=xQueueCreate(2,sizeof(UartCommand));
 uart_reply_queue=xQueueCreate(1,sizeof(UartReply));
 uart_evt_queue=xQueueCreate(16,sizeof(PreAdcDiagEvent));
 uart_diag_queue=xQueueCreate(4,128);
 if(!uart_cmd_queue||!uart_reply_queue||!uart_evt_queue||!uart_diag_queue||xTaskCreate(uart_command_task,"uartcmd",2048,NULL,4,&uart_command_handle)!=pdPASS)return 1;
#ifdef LFN_STORAGE_RUNTIME
 if(!lfn_storage_runtime_start())return 1;
 if(!lfn_storage_runtime_observe(&services))return 1;
 if(!capture_application_attach_history(&application,lfn_storage_runtime_history()))return 1;
#endif
 if(xTaskCreate(capture_entry,"capture",4096,NULL,6,&capture_handle)!=pdPASS)return 1;
#ifdef LFN_NETWORK
 network_capture_owner_init();
#endif
 /* Explicit request only: no DDR initializer, implicit session or auto START. */
 vTaskStartScheduler();return 1;
}
