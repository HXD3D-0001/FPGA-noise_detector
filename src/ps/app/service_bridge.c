#include "service_bridge.h"
#include <string.h>
static void inc(uint64_t*n){if(*n!=UINT64_MAX)(*n)++;}
static bool valid(const LfnServiceBridge*b){return b&&b->initialized;}
static bool qlock(LfnServiceBridge*b){return !b->locks.queue_lock||b->locks.queue_lock(b->locks.context);}
static void qunlock(LfnServiceBridge*b){if(b->locks.queue_unlock)b->locks.queue_unlock(b->locks.context);}
static bool slock(LfnServiceBridge*b){return !b->locks.snapshot_lock||b->locks.snapshot_lock(b->locks.context);}
static void sunlock(LfnServiceBridge*b){if(b->locks.snapshot_unlock)b->locks.snapshot_unlock(b->locks.context);}
bool lfn_service_bridge_init(LfnServiceBridge*b,const LfnServiceLocks*l){
 if(!b||!l)return false;
 unsigned supplied=(l->queue_lock!=NULL)+(l->queue_unlock!=NULL)+(l->snapshot_lock!=NULL)+(l->snapshot_unlock!=NULL);
 if(supplied!=0&&supplied!=4)return false;
 memset(b,0,sizeof(*b));b->locks=*l;b->next_delivery=b->next_snapshot=1;b->initialized=true;return true;
}
bool lfn_service_producer_observer(LfnServiceBridge*b,const LfnServiceProducerObserver*o){
 if(!valid(b))return false;
 b->observer=o?*o:(LfnServiceProducerObserver){0};return true;
}
static void push(LfnServiceBridge*b,const LfnServiceMessage*m){
 b->queue[(b->head+b->count)%LFN_SERVICE_ENERGY_SLOTS]=*m;b->count++;
 if(b->count>b->health.queue_highwater)b->health.queue_highwater=b->count;
}
static void lost(LfnServiceBridge*b,uint64_t n,uint32_t reason){
 if(!b->pending_loss){b->loss_first=n;b->pending_loss=true;b->loss_reason=0;}
 b->loss_end=n+1;b->loss_reason|=reason;inc(&b->health.dropped);
}
static bool flush_locked(LfnServiceBridge*b){
 if(!b->pending_loss)return true;
 if(b->count==LFN_SERVICE_ENERGY_SLOTS)return false;
 LfnServiceMessage message={0};message.kind=SERVICE_DELIVERY_LOSS;
 message.first_delivery=b->loss_first;message.end_delivery=b->loss_end;message.loss_reason=b->loss_reason;
 push(b,&message);b->pending_loss=false;inc(&b->health.loss_notices);return true;
}
static bool identity_ok(const LfnServiceIdentity*i,const CaptureEnergyMessage*m){
 if(!i||!m||!i->session_id||!i->capture_id||!i->config.config_id||i->config.mode!=0||
    i->config.source>1||i->config.channel>1||i->config.pattern>3||
    m->record.capture_id!=i->capture_id||m->record.config_id!=i->config.config_id)return false;
 if(m->gap)return m->first_missing_seq<m->end_missing_seq&&m->reason&&!(m->reason&~(ENERGY_GAP_SOURCE|ENERGY_GAP_POOL));
 return m->record.seq!=UINT64_MAX&&m->record.first_raw<=UINT64_MAX-12288&&m->record.energy<=(UINT64_C(1)<<62)&&
        (m->record.flags&LFN_VALID)&&!(m->record.flags&~(uint32_t)(LFN_VALID|LFN_SATURATION));
}
LfnServiceResult lfn_service_energy_offer(LfnServiceBridge*b,const LfnServiceIdentity*i,const CaptureEnergyMessage*m){
 if(!valid(b))return SERVICE_INVALID;
 if(!identity_ok(i,m)){inc(&b->health.invalid);return SERVICE_INVALID;}
 if(b->observer.energy)b->observer.energy(b->observer.context,i,m);
 if(b->next_delivery==UINT64_MAX){b->health.delivery_exhausted=true;inc(&b->health.unsequenced);return SERVICE_EXHAUSTED;}
 uint64_t n=b->next_delivery++;inc(&b->health.attempted);
 if(!qlock(b)){lost(b,n,LFN_DELIVERY_LOCK_BUSY);return SERVICE_BUSY;}
 if(!flush_locked(b)||b->count==LFN_SERVICE_ENERGY_SLOTS){
  qunlock(b);lost(b,n,LFN_DELIVERY_QUEUE_FULL);return SERVICE_BUSY;
 }
 LfnServiceMessage message={0};message.kind=SERVICE_ENERGY;message.first_delivery=n;message.end_delivery=n+1;
 message.identity=*i;message.energy=*m;push(b,&message);inc(&b->health.queued);qunlock(b);return SERVICE_OK;
}
LfnServiceResult lfn_service_flush(LfnServiceBridge*b){
 if(!valid(b))return SERVICE_INVALID;
 if(!b->pending_loss)return SERVICE_EMPTY;
 if(!qlock(b))return SERVICE_BUSY;
 bool ok=flush_locked(b);qunlock(b);return ok?SERVICE_OK:SERVICE_BUSY;
}
LfnServiceResult lfn_service_consumer_claim(LfnServiceBridge*b,const void*owner){
 if(!valid(b)||!owner)return SERVICE_INVALID;
 if(!qlock(b))return SERVICE_BUSY;
 bool ok=!b->consumer_owner||b->consumer_owner==owner;if(ok)b->consumer_owner=owner;qunlock(b);return ok?SERVICE_OK:SERVICE_BUSY;
}
LfnServiceResult lfn_service_consumer_release(LfnServiceBridge*b,const void*owner){
 if(!valid(b)||!owner)return SERVICE_INVALID;
 if(!qlock(b))return SERVICE_BUSY;
 LfnServiceResult r;
 if(b->consumer_owner!=owner)r=SERVICE_INVALID;
 else if(b->count||b->pending_loss)r=SERVICE_BUSY;
 else{b->consumer_owner=NULL;r=SERVICE_OK;}
 qunlock(b);return r;
}
LfnServiceResult lfn_service_energy_take_owned(LfnServiceBridge*b,const void*owner,LfnServiceMessage*out){
 if(!valid(b)||!out)return SERVICE_INVALID;
 if(!qlock(b))return SERVICE_BUSY;
 if(b->consumer_owner!=owner){qunlock(b);return SERVICE_BUSY;}
 if(!b->count){qunlock(b);return SERVICE_EMPTY;}
 *out=b->queue[b->head];b->head=(b->head+1)%LFN_SERVICE_ENERGY_SLOTS;b->count--;qunlock(b);return SERVICE_OK;
}
LfnServiceResult lfn_service_energy_take(LfnServiceBridge*b,LfnServiceMessage*out){return lfn_service_energy_take_owned(b,NULL,out);}
LfnServiceResult lfn_service_snapshot_publish(LfnServiceBridge*b,uint64_t now){
 if(!valid(b))return SERVICE_INVALID;
 b->staging.published_us=now;
 if(b->observer.snapshot)b->observer.snapshot(b->observer.context,&b->staging);
 if(b->next_snapshot==UINT64_MAX){b->health.snapshot_exhausted=true;return SERVICE_EXHAUSTED;}
 if(!slock(b)){inc(&b->health.snapshot_skipped);return SERVICE_BUSY;}
 b->staging.version=b->next_snapshot++;
 if(b->next_snapshot==UINT64_MAX)b->health.snapshot_exhausted=true;
 b->staging.transport=b->health;b->staging.transport.pending_loss=b->pending_loss;
 b->published=b->staging;b->have_snapshot=true;sunlock(b);return SERVICE_OK;
}
LfnServiceResult lfn_service_snapshot_copy(LfnServiceBridge*b,LfnServiceSnapshot*out){
 if(!valid(b)||!out)return SERVICE_INVALID;
 if(!slock(b))return SERVICE_BUSY;
 if(!b->have_snapshot){sunlock(b);return SERVICE_EMPTY;}
 *out=b->published;sunlock(b);return SERVICE_OK;
}

LfnServiceResult lfn_service_consumer_empty(LfnServiceBridge*b,const void*owner){
 if(!valid(b)||!owner)return SERVICE_INVALID;
 if(!qlock(b))return SERVICE_BUSY;
 LfnServiceResult r=b->consumer_owner!=owner?SERVICE_INVALID:b->count||b->pending_loss?SERVICE_BUSY:SERVICE_EMPTY;qunlock(b);return r;
}
