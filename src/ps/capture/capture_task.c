#include "capture_task.h"
#include <string.h>
static uint16_t get16(const uint8_t *b){return (uint16_t)(b[0]|((uint16_t)b[1]<<8));}
static uint32_t get32(const uint8_t *b){return (uint32_t)b[0]|((uint32_t)b[1]<<8)|((uint32_t)b[2]<<16)|((uint32_t)b[3]<<24);}
static bool config_ok(const CaptureConfig *cfg) {
    return cfg && cfg->session_id && cfg->capture_id && cfg->config_id && cfg->origin_raw<=UINT64_MAX-1679;
}
static unsigned required_mask(const CaptureTask *c) {
    return c->config.raw_mode?CAPTURE_RAW_MASK:CAPTURE_MONITOR_MASK;
}
static bool stream_ready(const CaptureTask *c,unsigned stream) {
    return !c->explicit_contract || ((required_mask(c)&(1u<<(stream-1))) &&
           (c->anchor_bound_mask&(1u<<(stream-1))));
}
static bool plan_config(const CapturePlan *p,CaptureConfig *cfg) {
    if(!p || !p->session_id || !p->capture_id || !p->config_id)return false;
    unsigned mask=p->raw_mode?CAPTURE_RAW_MASK:CAPTURE_MONITOR_MASK;
    for(unsigned i=0;i<4;i++)if((mask&(1u<<i)) && p->first_seq[i]==UINT64_MAX)return false;
    *cfg=(CaptureConfig){.session_id=p->session_id,.capture_id=p->capture_id,
        .config_id=p->config_id,.raw_mode=p->raw_mode};
    memcpy(cfg->first_seq,p->first_seq,sizeof(cfg->first_seq));return true;
}
static bool reject(CaptureTask *c,CaptureReject r,const PacketHeader *h) {
    c->rejects[r]++;if(c->hooks.on_reject)c->hooks.on_reject(c->hooks.context,r,h);return false;
}
bool capture_submit(CaptureTask *c,unsigned flow,const uint8_t *bytes,size_t n,bool dma_ok) {
    if(!c || flow>1)return false;
    if(c->explicit_contract && !stream_ready(c,flow==1?LFN_PCM:(c->config.raw_mode?LFN_RAW:LFN_FFT)))
        return reject(c,CAPTURE_CONTRACT,NULL);
    if(!dma_ok)return reject(c,CAPTURE_DMA_ERROR,NULL);
    if(!bytes || n<64 || n>LFN_MAX_PACKET)return reject(c,CAPTURE_LENGTH,NULL);
    uint16_t type=get16(bytes+6);size_t expected=type==LFN_PCM?1088:32832;
    if(type<LFN_FFT || type>LFN_RAW)return reject(c,CAPTURE_PROTOCOL,NULL);
    if(n!=expected || get32(bytes+12)!=n-64)return reject(c,CAPTURE_LENGTH,NULL);
    uint32_t crc=crc32c_extend(crc32c(bytes,60),bytes+64,n-64);
    if(crc!=get32(bytes+60))return reject(c,CAPTURE_CRC,NULL);
    PacketView p;
    if(!packet_decode(bytes,n,&p))return reject(c,CAPTURE_PROTOCOL,NULL);
    const PacketHeader *h=&p.header;
    if(h->capture_id!=c->config.capture_id || h->config_id!=c->config.config_id ||
       (flow==1 && type!=LFN_PCM) || (flow==0 && type!=(c->config.raw_mode?LFN_RAW:LFN_FFT)) ||
       (c->config.raw_mode && flow==1))return reject(c,CAPTURE_IDENTITY,h);
    if(!(h->flags&LFN_VALID) || (h->flags&(LFN_WARMUP|LFN_GAP|LFN_PROTOCOL_ERROR|LFN_CLOCK_ERROR)))return reject(c,CAPTURE_FLAGS,h);
    unsigned i=type-1;uint64_t first=c->first_raw[i];
    uint64_t hop=type==LFN_FFT?UINT64_C(98304):type==LFN_PCM?UINT64_C(12288):UINT64_C(8192);
    if(h->first_raw_sample_index<first)return reject(c,CAPTURE_RANGE,h);
    uint64_t delta=h->first_raw_sample_index-first;
    if(delta%hop || delta/hop>UINT64_MAX-c->config.first_seq[i] ||
       c->config.first_seq[i]+delta/hop!=h->stream_seq || h->stream_seq==UINT64_MAX)return reject(c,CAPTURE_RANGE,h);
    if(h->stream_seq<c->next_seq[i])return reject(c,CAPTURE_REORDER,h);
    if(h->stream_seq>c->next_seq[i]) {
        c->missing[i]+=h->stream_seq-c->next_seq[i];
        if(c->hooks.on_gap)c->hooks.on_gap(c->hooks.context,type,c->next_seq[i],h->stream_seq);
    }
    c->next_seq[i]=h->stream_seq+1;
    if(type!=LFN_PCM) {
        BlockHandle handle;uint8_t *copy;
        if(c->hooks.pool_lock && !c->hooks.pool_lock(c->hooks.context))return reject(c,CAPTURE_POOL_BUSY,h);
        if(!block_pool_acquire(c->pool,&handle,&copy)) {
            if(c->hooks.pool_unlock)c->hooks.pool_unlock(c->hooks.context);
            return reject(c,CAPTURE_POOL_FULL,h);
        }
        memcpy(copy,bytes,n);
        if(!block_pool_publish(c->pool,handle,n)) {
            (void)block_pool_release(c->pool,handle);
            if(c->hooks.pool_unlock)c->hooks.pool_unlock(c->hooks.context);
            return reject(c,CAPTURE_POOL_FULL,h);
        }
        if(c->hooks.pool_unlock)c->hooks.pool_unlock(c->hooks.context);
    }
    c->accepted[i]++;
    if(c->hooks.on_packet)c->hooks.on_packet(c->hooks.context,&p);
    /* p.payload is now dead to callbacks; no object with this pointer queued. */
    return true;
}
static void consume(void *v,DmaRx *r,DmaToken t,const uint8_t *b,size_t n,bool ok) {
    CaptureTask *c=v;uint64_t start=c->hooks.monotonic_us(c->hooks.context);
    (void)capture_submit(c,r==c->flows[1]?1:0,b,n,ok);
    if(dma_rx_release(r,t)) {
        DmaToken next;
        if(!r->submissions_paused && (!dma_rx_prepare(r,t.slot,&next) || !dma_rx_submit(r,next)))reject(c,CAPTURE_REARM_FAILED,NULL);
    } else {
        (void)dma_rx_quarantine_cpu(r,t);reject(c,CAPTURE_REARM_FAILED,NULL);
    }
    uint64_t end=c->hooks.monotonic_us(c->hooks.context);
    if(end>=start && end-start>c->max_packet_us)c->max_packet_us=end-start;
}
bool capture_task_set_config(CaptureTask *c,const CaptureConfig *cfg) {
    if(!c || !config_ok(cfg))return false;
    for(unsigned f=0;f<2;f++) {
        DmaRx *r=c->flows[f];if(r->resetting || r->fault)return false;
        for(size_t i=0;i<r->count;i++)if(r->slots[i].state!=DMA_FREE)return false;
    }
    if(c->hooks.pool_lock && !c->hooks.pool_lock(c->hooks.context))return false;
    bool pool_free=block_pool_free_count(c->pool)==LFN_BLOCK_SLOTS;
    if(c->hooks.pool_unlock)c->hooks.pool_unlock(c->hooks.context);
    if(!pool_free || c->energy_count || c->energy_pending_valid)return false;
    c->config=*cfg;c->explicit_contract=false;c->anchor_bound_mask=(uint8_t)required_mask(c);
    for(unsigned i=0;i<4;i++)c->first_raw[i]=cfg->origin_raw+(i==2?0:1679);
    for(unsigned i=0;i<3;i++){c->next_seq[i]=cfg->first_seq[i];c->accepted[i]=0;c->missing[i]=0;}
    c->energy_next_seq=cfg->first_seq[3];
    return true;
}
bool capture_task_init(CaptureTask *c,DmaRx *a,DmaRx *b,BlockPool *p,const CaptureConfig *cfg,const CaptureHooks *h) {
    if(!c || !a || !b || a==b || !p || !config_ok(cfg) || !h || !h->monotonic_us || !h->notify_task || (!!h->pool_lock!=!!h->pool_unlock))return false;
    memset(c,0,sizeof(*c));c->flows[0]=a;c->flows[1]=b;c->pool=p;c->hooks=*h;
    if(!capture_task_set_config(c,cfg))return false;
    a->consume=consume;a->consumer_context=c;b->consume=consume;b->consumer_context=c;return true;
}
bool capture_task_init_plan(CaptureTask *c,DmaRx *a,DmaRx *b,BlockPool *pool,const CapturePlan *p,const CaptureHooks *h) {
    CaptureConfig cfg;if(!plan_config(p,&cfg))return false;
    if(!capture_task_init(c,a,b,pool,&cfg,h))return false;
    c->explicit_contract=true;c->anchor_bound_mask=0;return true;
}
bool capture_task_prepare(CaptureTask *c,const CapturePlan *p) {
    CaptureConfig cfg;if(!c || !plan_config(p,&cfg))return false;
    /* Failed START consumes its reservation too. The caller supplies the real
     * next reservation; this API only prevents rollback/reuse within a session. */
    if(p->session_id==c->config.session_id && p->capture_id<=c->config.capture_id)return false;
    if(!capture_task_set_config(c,&cfg))return false;
    c->explicit_contract=true;c->anchor_bound_mask=0;return true;
}
bool capture_task_check_anchor(const CaptureTask *c,const CaptureAnchor *a) {
    if(!c || !a || !c->explicit_contract || a->stream<1 || a->stream>4 ||
       a->session_id!=c->config.session_id || a->capture_id!=c->config.capture_id ||
       a->config_id!=c->config.config_id)return false;
    unsigned i=a->stream-1,bit=1u<<i;
    uint64_t span=a->stream==LFN_FFT?UINT64_C(196608):a->stream==LFN_RAW?UINT64_C(8192):UINT64_C(12288);
    if(!(required_mask(c)&bit) || a->first_seq!=c->config.first_seq[i] || a->first_raw>UINT64_MAX-span)return false;
    if(c->anchor_bound_mask&bit)return c->first_raw[i]==a->first_raw;
    return true;
}
bool capture_task_bind_anchor(CaptureTask *c,const CaptureAnchor *a) {
    if(!capture_task_check_anchor(c,a))return false;
    unsigned i=a->stream-1;
    c->first_raw[i]=a->first_raw;c->anchor_bound_mask|=(uint8_t)(1u<<i);
    return true;
}
uint8_t capture_task_pending_anchors(const CaptureTask *c) {
    return !c || !c->explicit_contract?0:(uint8_t)(required_mask(c)&~c->anchor_bound_mask);
}
bool capture_task_received_through(const CaptureTask *c,uint64_t last) {
    if(!c || !c->explicit_contract || capture_task_pending_anchors(c))return false;
    unsigned mask=required_mask(c);
    for(unsigned i=0;i<4;i++)if(mask&(1u<<i)) {
        uint64_t stride=i==2?1:48, samples=i==0?4096:i==2?8192:256;
        uint64_t hop=(i==0?2048:i==3?128:samples)*stride;
        uint64_t tail=(samples-1)*stride,expected=0;
        if(last>=c->first_raw[i] && last-c->first_raw[i]>=tail)
            expected=(last-c->first_raw[i]-tail)/hop+1;
        uint64_t next=i==3?c->energy_next_seq:c->next_seq[i];
        if(next<c->config.first_seq[i] || next-c->config.first_seq[i]!=expected)return false;
    }
    return true;
}
bool capture_task_poll_due(const CaptureTask *c,uint64_t now_us) {
    return c && (!c->has_round || now_us<c->last_round_us || now_us-c->last_round_us>=CAPTURE_POLL_US);
}
size_t capture_task_run(CaptureTask *c) {
    if(!c)return 0;
    uint64_t start=c->hooks.monotonic_us(c->hooks.context);
    if(c->has_round && start>=c->last_round_us && start-c->last_round_us>c->max_service_interval_us)c->max_service_interval_us=start-c->last_round_us;
    c->last_round_us=start;c->has_round=true;
    unsigned counts[2]={0,0};
    bool empty[2]={!stream_ready(c,c->config.raw_mode?LFN_RAW:LFN_FFT),!stream_ready(c,LFN_PCM)};
    size_t total=0;
    unsigned energies=0;bool energy_empty=!c->hooks.energy_read || !stream_ready(c,CAPTURE_STREAM_ENERGY);
    while(counts[0]<CAPTURE_PACKETS_PER_FLOW || counts[1]<CAPTURE_PACKETS_PER_FLOW) {
        uint64_t now=c->hooks.monotonic_us(c->hooks.context);
        if(now<start || now-start>=CAPTURE_ROUND_US)break;
        /* One energy transaction before each PCM/FFT pair prevents starvation
         * when the shared 2ms budget is exhausted by a persistent DMA backlog. */
        if(!energy_empty && energies<CAPTURE_ENERGY_PER_ROUND && (total%2)==0) {
            PlEnergy e;
            if(c->hooks.energy_read(c->hooks.context,&e)){(void)capture_energy_submit(c,&e);energies++;}
            else energy_empty=true;
            now=c->hooks.monotonic_us(c->hooks.context);
            if(now<start || now-start>=CAPTURE_ROUND_US)break;
        }
        unsigned f=c->next_flow;c->next_flow^=1;
        if(counts[f]==CAPTURE_PACKETS_PER_FLOW || empty[f]) {
            if((empty[0] || counts[0]==CAPTURE_PACKETS_PER_FLOW) && (empty[1] || counts[1]==CAPTURE_PACKETS_PER_FLOW))break;
            continue;
        }
        size_t n=dma_rx_drain(c->flows[f],1);counts[f]+=(unsigned)n;total+=n;if(!n)empty[f]=true;
    }
    while(!energy_empty && energies<CAPTURE_ENERGY_PER_ROUND) {
        uint64_t now=c->hooks.monotonic_us(c->hooks.context);
        PlEnergy e;
        if(now<start || now-start>=CAPTURE_ROUND_US)break;
        if(!c->hooks.energy_read(c->hooks.context,&e)){energy_empty=true;break;}
        (void)capture_energy_submit(c,&e);
        energies++;
    }
    uint64_t end=c->hooks.monotonic_us(c->hooks.context);
    if(end>=start && end-start>c->max_round_us)c->max_round_us=end-start;
    /* Re-notification conservatively follows exhausted budget. Backend has no
     * unbounded peek/drain: an extra empty pass is safe and bounded. */
    if(!empty[0] || !empty[1] || !energy_empty)c->hooks.notify_task(c->hooks.context);
    return total;
}
static void energy_push(CaptureTask *c,CaptureEnergyMessage m) {
    c->energy_queue[(c->energy_head+c->energy_count)%CAPTURE_ENERGY_SLOTS]=m;
    if(++c->energy_count>c->energy_highwater)c->energy_highwater=c->energy_count;
}
static void energy_gap(CaptureTask *c,uint64_t first,uint64_t end,uint32_t reason) {
    if(!c->energy_pending_valid) {
        memset(&c->energy_pending,0,sizeof(c->energy_pending));
        c->energy_pending.gap=true;c->energy_pending.record.capture_id=c->config.capture_id;
        c->energy_pending.record.config_id=c->config.config_id;
        c->energy_pending.first_missing_seq=first;c->energy_pending.end_missing_seq=end;
        c->energy_pending.reason=reason;c->energy_pending_valid=true;
    } else {
        c->energy_pending.end_missing_seq=end;c->energy_pending.reason|=reason;
    }
}
bool capture_energy_submit(CaptureTask *c,const PlEnergy *e) {
    if(!c || !e)return false;
    if(!stream_ready(c,CAPTURE_STREAM_ENERGY)){c->energy_rejected++;return false;}
    uint64_t first=c->first_raw[3];
    if(c->config.raw_mode || e->capture_id!=c->config.capture_id || e->config_id!=c->config.config_id ||
       e->seq==UINT64_MAX || e->seq<c->energy_next_seq || e->first_raw<first ||
       (e->first_raw-first)%6144 || (e->first_raw-first)/6144>UINT64_MAX-c->config.first_seq[3] ||
       c->config.first_seq[3]+(e->first_raw-first)/6144!=e->seq ||
       e->first_raw>UINT64_MAX-12288 || e->energy>(UINT64_C(1)<<62) ||
       !(e->flags&LFN_VALID) || (e->flags&~(uint32_t)(LFN_VALID|LFN_SATURATION))) {
        c->energy_rejected++;return false;
    }
    if(e->seq>c->energy_next_seq) {
        c->energy_missing+=e->seq-c->energy_next_seq;
        energy_gap(c,c->energy_next_seq,e->seq,ENERGY_GAP_SOURCE);
    }
    c->energy_next_seq=e->seq+1;
    if(c->energy_pending_valid && c->energy_count<CAPTURE_ENERGY_SLOTS) {
        energy_push(c,c->energy_pending);c->energy_pending_valid=false;
    }
    if(c->energy_count==CAPTURE_ENERGY_SLOTS) {
        c->energy_dropped++;energy_gap(c,e->seq,e->seq+1,ENERGY_GAP_POOL);return false;
    }
    CaptureEnergyMessage message={0};message.record=*e;energy_push(c,message);return true;
}
bool capture_energy_take(CaptureTask *c,CaptureEnergyMessage *e) {
    if(!c || !e)return false;
    if(c->energy_count) {
        *e=c->energy_queue[c->energy_head];c->energy_head=(c->energy_head+1)%CAPTURE_ENERGY_SLOTS;c->energy_count--;return true;
    }
    if(c->energy_pending_valid){*e=c->energy_pending;c->energy_pending_valid=false;return true;}
    return false;
}
