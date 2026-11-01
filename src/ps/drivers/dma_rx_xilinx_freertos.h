#ifndef LFN_DMA_RX_XILINX_FREERTOS_H
#define LFN_DMA_RX_XILINX_FREERTOS_H
#include "dma_rx_xilinx.h"
#include "FreeRTOS.h"
#include "task.h"
#include "xscugic.h"
typedef struct { DmaXilinx *backend; DmaRx *rx; TaskHandle_t capture_task; bool connected; } DmaXilinxRtos;
/* Use as DmaXilinxPlatform.notify_from_isr, with platform.context=&adapter.
 * Connect after scheduler tick is verified; no allocation/recovery in ISR. */
void dma_rx_xilinx_notify_from_isr(void *,uint32_t);
bool dma_rx_xilinx_rtos_connect(DmaXilinxRtos *,DmaXilinx *,DmaRx *,TaskHandle_t,
    XScuGic *actual_port_gic,uint32_t actual_generated_interrupt_id);
#ifdef SDT
/* Modern BSP shares the RTOS-owned interrupt wrapper/controller instance. */
bool dma_rx_xilinx_rtos_connect_sdt(DmaXilinxRtos *,DmaXilinx *,DmaRx *,TaskHandle_t,
    uint32_t genuine_encoded_interrupt,UINTPTR genuine_interrupt_parent);
#endif
#endif
