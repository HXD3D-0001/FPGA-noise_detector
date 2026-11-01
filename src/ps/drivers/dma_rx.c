#include "dma_rx.h"
#include <string.h>
static bool matches(const DmaRx *r,DmaToken t) {
    return r && t.slot<r->count && t.epoch==r->epoch && t.generation!=0 && r->slots[t.slot].generation==t.generation;
}
void dma_rx_pause_submissions(DmaRx*r,bool paused){if(r)r->submissions_paused=paused;}
bool dma_rx_init(DmaRx *r,const DmaBackend *b,void *m,size_t c,size_t n) {
    if(!r || !b || !m || (uintptr_t)m%64 || !c || c%64 || !n || n>DMA_RX_MAX_SLOTS ||
       c>SIZE_MAX/n || (uintptr_t)m>UINTPTR_MAX-c*n || !b->clean || !b->invalidate || !b->barrier ||
       !b->submit || !b->poll || !b->reset_start || !b->reset_done || !b->irq_ack || !b->notify_from_isr)return false;
    memset(r,0,sizeof(*r));r->backend=*b;r->memory=m;r->capacity=c;r->count=n;r->epoch=1;return true;
}
bool dma_rx_prepare(DmaRx *r,size_t n,DmaToken *t) {
    if(!r || !t || r->resetting || r->fault || r->submissions_paused || n>=r->count || r->slots[n].state!=DMA_FREE || r->slots[n].generation==UINT64_MAX)return false;
    DmaSlot *s=&r->slots[n];s->generation++;s->length=0;s->packet_ok=false;s->state=DMA_PREPARED;
    *t=(DmaToken){(uint16_t)n,r->epoch,s->generation};return true;
}
bool dma_rx_submit(DmaRx *r,DmaToken t) {
    if(!matches(r,t) || r->resetting || r->fault || r->submissions_paused || r->slots[t.slot].state!=DMA_PREPARED)return false;
    uint8_t *data=r->memory+t.slot*r->capacity;
    /* CPU's dirty old lines are retired BEFORE hardware ownership. Descriptor
     * synchronization occurs inside backend submit using real driver stride. */
    r->backend.clean(r->backend.context,data,r->capacity);
    r->backend.barrier(r->backend.context);
    if(!r->backend.submit(r->backend.context,t,data,r->capacity)){r->submission_failures++;return false;}
    r->slots[t.slot].state=DMA_OWNED;return true;
}
bool dma_rx_complete(DmaRx *r,DmaCompletion c,DmaToken *t,const uint8_t **b,size_t *n,bool *ok) {
    if(!r || !t || !b || !n || !ok)return false;
    *b=NULL;*n=0;*ok=false;
    if(!matches(r,c.token) || r->resetting || r->slots[c.token.slot].state!=DMA_OWNED){r->stale_completions++;return false;}
    DmaSlot *s=&r->slots[c.token.slot];uint8_t *data=r->memory+c.token.slot*r->capacity;
    /* NEVER clean received data: stale dirty lines would overwrite DMA output. */
    r->backend.invalidate(r->backend.context,data,r->capacity);
    r->backend.barrier(r->backend.context);
    s->state=DMA_CPU_OWNED;s->length=c.actual_length<=r->capacity?c.actual_length:0;
    s->packet_ok=c.complete_packet && !c.error && c.actual_length>0 && c.actual_length<=r->capacity;
    if(!s->packet_ok)r->bad_completions++;
    *t=c.token;*b=data;*n=s->length;*ok=s->packet_ok;return true;
}
bool dma_rx_release(DmaRx *r,DmaToken t) {
    if(!matches(r,t) || r->slots[t.slot].state!=DMA_CPU_OWNED)return false;
    if(r->backend.reclaim && !r->backend.reclaim(r->backend.context,t))return false;
    r->slots[t.slot].state=DMA_FREE;r->slots[t.slot].length=0;r->slots[t.slot].packet_ok=false;return true;
}
bool dma_rx_quarantine_cpu(DmaRx *r,DmaToken t) {
    if(!matches(r,t) || r->slots[t.slot].state!=DMA_CPU_OWNED)return false;
    r->slots[t.slot].state=DMA_QUARANTINED;r->fault=true;return true;
}
size_t dma_rx_drain(DmaRx *r,size_t max_packets) {
    if(!r || !r->consume || r->resetting || r->fault)return 0;
    size_t polled=0;
    /* Stale events count toward budget: a faulty backend cannot spin forever. */
    while(polled<max_packets) {
        DmaCompletion c;if(!r->backend.poll(r->backend.context,&c))break;polled++;
        DmaToken t;const uint8_t *b;size_t n;bool ok;
        if(dma_rx_complete(r,c,&t,&b,&n,&ok))r->consume(r->consumer_context,r,t,b,n,ok);
    }
    return polled;
}
bool dma_rx_reset_begin(DmaRx *r) {
    if(!r || r->resetting || r->epoch==UINT64_MAX)return false;
    /* A synchronous consumer must release its borrow before resetting. */
    for(size_t i=0;i<r->count;i++)if(r->slots[i].state==DMA_CPU_OWNED)return false;
    r->resetting=true;
    for(size_t i=0;i<r->count;i++)if(r->slots[i].state!=DMA_FREE)r->slots[i].state=DMA_QUARANTINED;
    if(!r->backend.reset_start(r->backend.context)){r->fault=true;return false;}
    r->fault=false; /* reset request accepted; memory remains quarantined */
    return true;
}
bool dma_rx_reset_poll(DmaRx *r) {
    if(!r || !r->resetting || r->fault || !r->backend.reset_done(r->backend.context))return false;
    r->backend.barrier(r->backend.context);r->epoch++;
    for(size_t i=0;i<r->count;i++){r->slots[i].state=DMA_FREE;r->slots[i].length=0;r->slots[i].packet_ok=false;}
    r->resetting=false;return true;
}
void dma_rx_isr(DmaRx *r) {
    if(!r)return;
    uint32_t s=r->backend.irq_ack(r->backend.context);
    /* Status lives in platform ISR-safe notification; no recovery here. */
    r->irq_status|=s;r->backend.notify_from_isr(r->backend.context,s);
}
