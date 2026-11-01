#include "dma_rx_xilinx.h"
#include "xil_cache.h"
#include <string.h>
size_t dma_rx_xilinx_bd_bytes(size_t n) {
    return n && n<=DMA_RX_MAX_SLOTS?(size_t)XAxiDma_BdRingMemCalc(XAXIDMA_BD_MINIMUM_ALIGNMENT,n):0;
}
static bool same_token(DmaToken a,DmaToken b){return a.slot==b.slot && a.epoch==b.epoch && a.generation==b.generation;}
static void clean(void *v,void *b,size_t n){DmaXilinx *x=v;x->platform.clean(x->platform.context,b,n);}
static void invalid(void *v,void *b,size_t n){DmaXilinx *x=v;x->platform.invalidate(x->platform.context,b,n);}
static void barrier(void *v){DmaXilinx *x=v;x->platform.barrier(x->platform.context);}
static bool ring_create(DmaXilinx *x) {
    XAxiDma_BdRingIntDisable(x->ring,XAXIDMA_IRQ_ALL_MASK);
    if(XAxiDma_BdRingCreate(x->ring,x->bd_physical,(UINTPTR)x->bd_memory,XAXIDMA_BD_MINIMUM_ALIGNMENT,(int)x->slots)!=XST_SUCCESS)return false;
    XAxiDma_Bd zero;memset(&zero,0,sizeof(zero));
    if(XAxiDma_BdRingClone(x->ring,&zero)!=XST_SUCCESS || XAxiDma_BdRingSetCoalesce(x->ring,1,0)!=XST_SUCCESS)return false;
    x->ring->Cyclic=0;
    if(XAxiDma_SelectCyclicMode(x->dma,XAXIDMA_DEVICE_TO_DMA,0)!=XST_SUCCESS)return false;
    if(x->ring->Separation!=(UINTPTR)dma_rx_xilinx_bd_bytes(1) || x->ring->HasStsCntrlStrm)return false;
    /* RingCreate clears descriptor memory only after hardware reset was proved.
     * Descriptor clean here precedes ownership; FromHw handles later invalidate. */
    clean(x,x->bd_memory,dma_rx_xilinx_bd_bytes(x->slots));barrier(x);
    memset(x->active,0,sizeof(x->active));memset(x->tokens,0,sizeof(x->tokens));
    x->post=NULL;x->started=false;return true;
}
static bool submit(void *v,DmaToken t,void *data,size_t cap) {
    DmaXilinx *x=v;
    if(x->fault || x->reset_requested || t.slot>=x->slots || cap!=x->capacity || x->active[t.slot])return false;
    XAxiDma_Bd *bd=NULL;
    if(XAxiDma_BdRingAlloc(x->ring,1,&bd)!=XST_SUCCESS)return false;
    XAxiDma_BdClear(bd);
    if(XAxiDma_BdSetBufAddr(bd,(UINTPTR)data)!=XST_SUCCESS ||
       XAxiDma_BdSetLength(bd,(u32)cap,x->ring->MaxTransferLen)!=XST_SUCCESS) {
        if(XAxiDma_BdRingUnAlloc(x->ring,1,bd)!=XST_SUCCESS){x->fault=true;x->owner->fault=true;}
        return false;
    }
    XAxiDma_BdSetCtrl(bd,0);XAxiDma_BdSetId(bd,t.slot+1u);
    x->tokens[t.slot]=t;x->active[t.slot]=bd;
    barrier(x);
    if(XAxiDma_BdRingToHw(x->ring,1,bd)!=XST_SUCCESS) {
        /* No ownership gamble on API failure: retain memory until reset. */
        x->fault=true;x->owner->fault=true;return false;
    }
    if(!x->started) {
        if(XAxiDma_BdRingStart(x->ring)!=XST_SUCCESS){x->fault=true;x->owner->fault=true;return false;}
        x->started=true;
    }
    return true;
}
static bool poll(void *v,DmaCompletion *c) {
    DmaXilinx *x=v;
    if(!c || x->fault || x->reset_requested || x->post)return false;
    if(x->started && !x->owner->submissions_paused) {
        u32 cr=XAxiDma_ReadReg(x->ring->ChanBase,XAXIDMA_CR_OFFSET);
        if(!(cr&XAXIDMA_CR_RUNSTOP_MASK) || (cr&XAXIDMA_CR_RESET_MASK)) {
            x->fault=true;return false;
        }
    }
    uint32_t irq=XAxiDma_BdRingGetIrq(x->ring);
    if(irq&XAXIDMA_IRQ_ERROR_MASK){x->fault=true;return false;}
    if(x->ring->HwCnt>0) {
        XAxiDma_Bd *head=x->ring->HwHead;
        invalid(x,head,(size_t)x->ring->Separation);barrier(x);
        uint32_t s=XAxiDma_BdGetSts(head);
        /* Official FromHw excludes partial packets, so a completed descriptor
         * without EOF cannot safely be freed via a one-BD callback. */
        if((s&XAXIDMA_BD_STS_ALL_ERR_MASK) ||
           ((s&XAXIDMA_BD_STS_COMPLETE_MASK) && !(s&XAXIDMA_BD_STS_RXEOF_MASK))) {x->fault=true;return false;}
    }
    XAxiDma_Bd *bd=NULL;
    if(XAxiDma_BdRingFromHw(x->ring,1,&bd)!=1)return false;
    uint32_t id=XAxiDma_BdGetId(bd);
    if(!id || id>x->slots || x->active[id-1]!=bd){x->fault=true;return false;}
    uint32_t s=XAxiDma_BdGetSts(bd);DmaToken t=x->tokens[id-1];
    x->post=bd;x->post_token=t;
    *c=(DmaCompletion){t,XAxiDma_BdGetActualLength(bd,x->ring->MaxTransferLen),
        (s&(XAXIDMA_BD_STS_RXSOF_MASK|XAXIDMA_BD_STS_RXEOF_MASK|XAXIDMA_BD_STS_COMPLETE_MASK))==
           (XAXIDMA_BD_STS_RXSOF_MASK|XAXIDMA_BD_STS_RXEOF_MASK|XAXIDMA_BD_STS_COMPLETE_MASK),
        (s&XAXIDMA_BD_STS_ALL_ERR_MASK)!=0};
    return true;
}
static bool reclaim(void *v,DmaToken t) {
    DmaXilinx *x=v;
    /* An error IRQ may race this completed CPU borrow. Its post-work BD can
     * still be freed safely; in-flight descriptors remain quarantined later. */
    if(x->reset_requested || !x->post || !same_token(t,x->post_token))return false;
    if(XAxiDma_BdRingFree(x->ring,1,x->post)!=XST_SUCCESS){x->fault=true;return false;}
    x->active[t.slot]=NULL;x->post=NULL;return true;
}
static bool reset_start(void *v) {
    DmaXilinx *x=v;if(x->reset_requested)return false;
    /* Caller MUST stop this producer and coordinate its partial AXIS packet.
     * This S2MM-only DMA is independent of the other engine. */
    XAxiDma_BdRingIntDisable(x->ring,XAXIDMA_IRQ_ALL_MASK);XAxiDma_Reset(x->dma);
    x->reset_requested=true;return true;
}
static bool reset_done(void *v) {
    DmaXilinx *x=v;
    if(!x->reset_requested || !XAxiDma_ResetIsDone(x->dma))return false;
    if(!ring_create(x)){x->fault=true;return false;}
    x->reset_requested=false;x->fault=false;x->irq_status=0;return true;
}
static uint32_t irq_ack(void *v) {
    DmaXilinx *x=v;uint32_t s=XAxiDma_BdRingGetIrq(x->ring);
    XAxiDma_BdRingAckIrq(x->ring,s);x->irq_status|=s;
    if(s&XAXIDMA_IRQ_ERROR_MASK)x->fault=true;
    return s;
}
static void notify(void *v,uint32_t s){DmaXilinx *x=v;x->platform.notify_from_isr(x->platform.context,s);}
bool dma_rx_xilinx_bind(DmaXilinx *x,DmaRx *r,XAxiDma *d,const XAxiDma_Config *cfg,
    void *data,size_t cap,size_t n,void *bd,UINTPTR phy,size_t bytes,const DmaXilinxPlatform *p) {
    size_t need=dma_rx_xilinx_bd_bytes(n);
    if(!x || !r || !d || !cfg || !p || !p->clean || !p->invalidate || !p->barrier || !p->notify_from_isr ||
       !d->Initialized || !cfg->HasSg || !cfg->HasS2Mm || cfg->HasMm2S || cfg->HasStsCntrlStrm ||
       cfg->HasS2MmDRE || cfg->S2MmDataWidth!=64 || cfg->S2MmNumChannels!=1 || cfg->MicroDmaMode ||
       cfg->SgLengthWidth!=23 || cfg->BaseAddr!=d->RegBase || !XAxiDma_HasSg(d) ||
       d->HasMm2S || !d->HasS2Mm || d->RxNumChannels!=1 || !need || !bd || (uintptr_t)bd%64 || phy%64 || bytes<need ||
       phy>UINTPTR_MAX-need || (uintptr_t)bd>UINTPTR_MAX-need || !cap || cap>UINT32_MAX || cap>SIZE_MAX/n ||
       !data || (uintptr_t)data>UINTPTR_MAX-cap*n)return false;
    uintptr_t begin=(uintptr_t)data,end=begin+cap*n,bd_begin=(uintptr_t)bd,bd_end=bd_begin+need;
    if((begin<bd_end && bd_begin<end) || (begin<phy+need && phy<end))return false;
    if(cfg->AddrWidth==32 && ((uint64_t)begin+(uint64_t)cap*n>UINT64_C(0x100000000) || (uint64_t)phy+need>UINT64_C(0x100000000)))return false;
    XAxiDma_BdRing *ring=XAxiDma_GetRxRing(d);
    if(ring->HasStsCntrlStrm || ring->HasDRE || ring->DataWidth!=8 || ring->MaxTransferLen<cap || ring->HwCnt || ring->PostCnt || ring->PreCnt)return false;
    memset(x,0,sizeof(*x));x->dma=d;x->ring=ring;x->owner=r;x->bd_memory=bd;x->bd_physical=phy;
    x->bd_bytes=bytes;x->slots=n;x->capacity=cap;x->platform=*p;
    DmaBackend backend={x,clean,invalid,barrier,submit,poll,reset_start,reset_done,irq_ack,notify,reclaim};
    if(!dma_rx_init(r,&backend,data,cap,n) || !ring_create(x))return false;
    return true;
}
#if defined(__arm__)
static void native_clean(void *v,void *b,size_t n){(void)v;Xil_DCacheFlushRange((INTPTR)b,(u32)n);}
static void native_invalid(void *v,void *b,size_t n){(void)v;Xil_DCacheInvalidateRange((INTPTR)b,(u32)n);}
static void native_barrier(void *v){(void)v;__asm__ __volatile__("dmb sy" ::: "memory");}
#endif
bool dma_rx_xilinx_cache_defaults(DmaXilinxPlatform *p) {
    if(!p)return false;
#if defined(__arm__)
    p->clean=native_clean;p->invalidate=native_invalid;p->barrier=native_barrier;return true;
#else
    return false;
#endif
}
bool dma_rx_xilinx_enable_irq(DmaXilinx *x,bool verified) {
    if(!x || !verified || x->fault || x->reset_requested)return false;
    XAxiDma_BdRingIntEnable(x->ring,XAXIDMA_IRQ_ALL_MASK);return true;
}
bool dma_rx_xilinx_recovery_required(const DmaXilinx *x){return x && x->fault;}
bool dma_rx_xilinx_halt(DmaXilinx*x){
    if(!x || !x->owner || !x->dma || !x->dma->Initialized || !x->ring || x->reset_requested)return false;
    dma_rx_pause_submissions(x->owner,true);
    u32 cr=XAxiDma_ReadReg(x->ring->ChanBase,XAXIDMA_CR_OFFSET);
    XAxiDma_WriteReg(x->ring->ChanBase,XAXIDMA_CR_OFFSET,cr&~XAXIDMA_CR_RUNSTOP_MASK);
    x->platform.barrier(x->platform.context);return true;
}
bool dma_rx_xilinx_drain_halt(DmaXilinx*x){
    if(!dma_rx_xilinx_halt(x))return false;
    /* After producer drain and complete-packet receipt, cancel empty SG
     * prefetch commands. RS0 alone can wait forever for another input packet.
     * This hardware reset does not reclaim tokens or rebuild the software ring;
     * that remains the subsequent tagged RESET owner's responsibility. */
    XAxiDma_Reset(x->dma);
    x->platform.barrier(x->platform.context);return true;
}
bool dma_rx_xilinx_idle(DmaXilinx*x){
    if(!x || !x->dma || !x->dma->Initialized || !x->ring || !x->owner ||
       !x->owner->submissions_paused || x->reset_requested || x->fault || x->owner->fault)return false;
    u32 cr=XAxiDma_ReadReg(x->ring->ChanBase,XAXIDMA_CR_OFFSET);
    u32 sr=XAxiDma_ReadReg(x->ring->ChanBase,XAXIDMA_SR_OFFSET);
    x->last_hw_busy=XAxiDma_Busy(x->dma,XAXIDMA_DEVICE_TO_DMA);
    /* PG021: HALTED implies DMA+SG operations stopped after RS0; IDLE is0
     * while HALTED. Native Busy (!IDLE) is recorded, not treated as in-flight.
     * Keep every owned token/BD intact even after this quiescence observation. */
    return !(cr&(XAXIDMA_CR_RUNSTOP_MASK|XAXIDMA_CR_RESET_MASK)) &&
           (sr&XAXIDMA_HALTED_MASK)!=0 && !(sr&(XAXIDMA_ERR_ALL_MASK|XAXIDMA_IRQ_ERROR_MASK));
}
