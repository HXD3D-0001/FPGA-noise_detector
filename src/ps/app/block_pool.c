#include "block_pool.h"
#include <string.h>
void block_pool_init(BlockPool *p){ if(p)memset(p,0,sizeof(*p)); }
static bool matches(const BlockPool *p,BlockHandle h) {
    return p && h.slot<LFN_BLOCK_SLOTS && h.generation!=0 && p->generation[h.slot]==h.generation;
}
bool block_pool_acquire(BlockPool *p,BlockHandle *h,uint8_t **b) {
    if(!p || !h || !b)return false;
    for(unsigned i=0;i<LFN_BLOCK_SLOTS;i++) if(p->state[i]==BLOCK_FREE && p->generation[i]!=UINT64_MAX) {
        p->generation[i]++;p->state[i]=BLOCK_PRODUCER;p->length[i]=0;
        *h=(BlockHandle){(uint16_t)i,p->generation[i]};*b=p->blocks[i].bytes;return true;
    }
    return false;
}
bool block_pool_publish(BlockPool *p,BlockHandle h,size_t n) {
    if(!matches(p,h) || p->state[h.slot]!=BLOCK_PRODUCER || !n || n>LFN_MAX_PACKET || p->count==LFN_BLOCK_SLOTS)return false;
    p->length[h.slot]=n;p->state[h.slot]=BLOCK_QUEUED;
    p->queue[(p->head+p->count)%LFN_BLOCK_SLOTS]=h;
    if(++p->count>p->highwater)p->highwater=p->count;
    return true;
}
bool block_pool_take(BlockPool *p,BlockHandle *h,const uint8_t **b,size_t *n) {
    if(!p || !h || !b || !n || !p->count)return false;
    *h=p->queue[p->head];p->head=(p->head+1)%LFN_BLOCK_SLOTS;p->count--;
    if(!matches(p,*h) || p->state[h->slot]!=BLOCK_QUEUED)return false;
    p->state[h->slot]=BLOCK_CONSUMER;*b=p->blocks[h->slot].bytes;*n=p->length[h->slot];return true;
}
bool block_pool_release(BlockPool *p,BlockHandle h) {
    if(!matches(p,h) || (p->state[h.slot]!=BLOCK_PRODUCER && p->state[h.slot]!=BLOCK_CONSUMER))return false;
    p->state[h.slot]=BLOCK_FREE;p->length[h.slot]=0;return true;
}
unsigned block_pool_free_count(const BlockPool *p) {
    unsigned n=0;if(p)for(unsigned i=0;i<LFN_BLOCK_SLOTS;i++)if(p->state[i]==BLOCK_FREE && p->generation[i]!=UINT64_MAX)n++;
    return n;
}
