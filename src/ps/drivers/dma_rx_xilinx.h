#ifndef LFN_DMA_RX_XILINX_H
#define LFN_DMA_RX_XILINX_H
#include "dma_rx.h"
#include "xaxidma.h"
typedef struct {
    void *context;
    void (*clean)(void *,void *,size_t);
    void (*invalidate)(void *,void *,size_t);
    void (*barrier)(void *);
    void (*notify_from_isr)(void *,uint32_t);
} DmaXilinxPlatform;
typedef struct {
    XAxiDma *dma;
    XAxiDma_BdRing *ring;
    DmaRx *owner;
    void *bd_memory;
    UINTPTR bd_physical;
    size_t bd_bytes,slots,capacity;
    DmaXilinxPlatform platform;
    XAxiDma_Bd *active[DMA_RX_MAX_SLOTS],*post;
    DmaToken tokens[DMA_RX_MAX_SLOTS],post_token;
    bool started,reset_requested;
    volatile bool fault;
    volatile uint32_t irq_status;
    uint32_t last_hw_busy; /* Native SG-tail IDLE diagnostic, not halt proof. */
} DmaXilinx;
size_t dma_rx_xilinx_bd_bytes(size_t slots);
/* Caller first initializes XAxiDma via genuine BSP LookupConfig/CfgInitialize.
 * This binds only S2MM SG, one packet/BD, status stream disabled, noncyclic.
 * Identity mapping data is required; BD virtual/physical mapping is explicit.
 * IRQ stays disabled. No GIC address or ID is guessed here. */
bool dma_rx_xilinx_bind(DmaXilinx *,DmaRx *,XAxiDma *,const XAxiDma_Config *,
    void *data,size_t capacity,size_t slots,void *bd_virtual,UINTPTR bd_physical,
    size_t bd_bytes,const DmaXilinxPlatform *);
/* ARM platform defaults use real Xil_DCache maintenance and DMB. Notification
 * callback is supplied by an adapter verified against actual FreeRTOS/GIC. */
bool dma_rx_xilinx_cache_defaults(DmaXilinxPlatform *);
bool dma_rx_xilinx_enable_irq(DmaXilinx *,bool gic_and_rtos_priority_verified);
bool dma_rx_xilinx_recovery_required(const DmaXilinx *);
/* Legacy RS0 request: pause submissions without resetting hardware or freeing
 * ownership. Empty SG prefetch may prevent actual HALTED; still poll idle. */
bool dma_rx_xilinx_halt(DmaXilinx *);
/* STOP drain barrier only, after producer drain and all complete packets have
 * been received. Cancel empty hardware prefetch by soft reset, retain all DMA
 * memory ownership and disable task rearming. Requires RS0/RESET0/HALTED1 without
 * fault; PG021 IDLE is0 while HALTED, so native Busy may remain true. This is
 * neither packet-completeness nor buffer-reuse proof. Only the separate tagged
 * reset operation may rebuild the ring and free software ownership. */
bool dma_rx_xilinx_drain_halt(DmaXilinx *);
bool dma_rx_xilinx_idle(DmaXilinx *); /* historical name; returns quiescence */
#endif
