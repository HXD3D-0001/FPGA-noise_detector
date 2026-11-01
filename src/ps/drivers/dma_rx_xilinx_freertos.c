#include "dma_rx_xilinx_freertos.h"
#ifdef SDT
#include "xinterrupt_wrap.h"
extern uintptr_t IntrControllerAddr;
#endif
#if configUSE_TASK_NOTIFICATIONS != 1
#error DMA capture requires genuine FreeRTOS task notifications
#endif
static void handler(void *v);
void dma_rx_xilinx_notify_from_isr(void *v,uint32_t status) {
    DmaXilinxRtos *a=v;(void)status;
    if(!a || !a->connected || !a->capture_task)return;
    BaseType_t wake=pdFALSE;vTaskNotifyGiveFromISR(a->capture_task,&wake);portYIELD_FROM_ISR(wake);
}
#ifdef SDT
bool dma_rx_xilinx_rtos_connect_sdt(DmaXilinxRtos *a,DmaXilinx *x,DmaRx *rx,TaskHandle_t task,uint32_t id,UINTPTR parent) {
    if(!a || !x || !rx || x->owner!=rx || !task || XGet_BaseAddr(parent)!=XGet_BaseAddr(IntrControllerAddr) ||
       XGet_IntcType(parent)!=XGet_IntcType(IntrControllerAddr) || XGet_IntrType(id)!=0 ||
       XGet_IntrId(id)+XGet_IntrOffset(id)>=XSCUGIC_MAX_NUM_INTR_INPUTS ||
       x->platform.context!=a || x->platform.notify_from_isr!=dma_rx_xilinx_notify_from_isr || xTaskGetTickCount()==0)return false;
    u8 priority=0,trigger=0;XGetPriorityTriggerType(id,&priority,&trigger,parent);
    uint32_t logical=(uint32_t)priority>>portPRIORITY_SHIFT;
    if(logical<configMAX_API_CALL_INTERRUPT_PRIORITY || logical>portLOWEST_USABLE_INTERRUPT_PRIORITY || trigger!=1)return false;
    a->backend=x;a->rx=rx;a->capture_task=task;a->connected=false;
    XDisableIntrId(id,parent);
    if(xPortInstallInterruptHandler((uint16_t)id,handler,a)!=pdPASS)return false;
    a->connected=true;
    if(!dma_rx_xilinx_enable_irq(x,true)){a->connected=false;(void)XDisconnectInterruptCntrl(id,parent);return false;}
    vPortEnableInterrupt((uint16_t)id);return true;
}
#endif
static void handler(void *v) {
    DmaXilinxRtos *a=v;
    if(a && a->connected)dma_rx_isr(a->rx);
}
bool dma_rx_xilinx_rtos_connect(DmaXilinxRtos *a,DmaXilinx *x,DmaRx *rx,TaskHandle_t task,XScuGic *gic,uint32_t id) {
    if(!a || !x || !rx || x->owner!=rx || !task || !gic || !gic->IsReady || id<32 || id>=XSCUGIC_MAX_NUM_INTR_INPUTS ||
       x->platform.context!=a || x->platform.notify_from_isr!=dma_rx_xilinx_notify_from_isr || xTaskGetTickCount()==0)return false;
    u8 priority=0,trigger=0;XScuGic_GetPriorityTriggerType(gic,id,&priority,&trigger);
    uint32_t logical_priority=(uint32_t)priority>>portPRIORITY_SHIFT;
    if(logical_priority<configMAX_API_CALL_INTERRUPT_PRIORITY || logical_priority>portLOWEST_USABLE_INTERRUPT_PRIORITY || trigger!=1)return false;
    a->backend=x;a->rx=rx;a->capture_task=task;a->connected=false;
    XScuGic_Disable(gic,id);
    if(XScuGic_Connect(gic,id,handler,a)!=XST_SUCCESS)return false;
    a->connected=true;
    if(!dma_rx_xilinx_enable_irq(x,true)){a->connected=false;XScuGic_Disconnect(gic,id);return false;}
    XScuGic_Enable(gic,id);return true;
}
