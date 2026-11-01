#include "capture_storage_route.h"
#include <string.h>
enum { FREE,QUEUED,PINNED,NONE=CAPTURE_ROUTE_SLOTS,ENERGY=5 };
static void inc(uint64_t*v){if(*v!=UINT64_MAX)(*v)++;}
static bool lock(CaptureStorageRoute*r){return !r->locks.lock||r->locks.lock(r->locks.context);}
static void unlock(CaptureStorageRoute*r){if(r->locks.unlock)r->locks.unlock(r->locks.context);}
static bool attached(CaptureStorageRoute*r){return r&&atomic_load_explicit(&r->attached,memory_order_acquire);}
static void publish(CaptureStorageRoute*r){r->published.source=r->producer;}
bool capture_storage_route_init(CaptureStorageRoute*r,uint64_t session,const StorageQueueLocks*l,const CaptureStorageSink*s){
 if(!r||!session||!l||!!l->lock!=!!l->unlock||!s||!s->energy||!s->psd||!s->pcm||!s->loss||!s->poll||!s->ack||!s->close)return false;
 memset(r,0,sizeof(*r));r->session_id=session;r->locks=*l;r->sink=*s;r->next_order=1;r->published.generation=1;r->active=NONE;atomic_init(&r->attached,false);return true;
}
StorageResult capture_storage_route_bind_result(CaptureStorageRoute*r,LfnServiceBridge*b){
 if(!r||!r->session_id||!b||attached(r))return STORE_INVALID;
 LfnServiceResult result=lfn_service_consumer_claim(b,r);if(result==SERVICE_BUSY)return STORE_BUSY;if(result!=SERVICE_OK)return STORE_INVALID;
 r->bridge=b;r->published.attached=true;atomic_store_explicit(&r->attached,true,memory_order_release);return STORE_OK;
}
bool capture_storage_route_bind(CaptureStorageRoute*r,LfnServiceBridge*b){return capture_storage_route_bind_result(r,b)==STORE_OK;}
static bool push(CaptureStorageRoute*r,unsigned kind,const LfnServiceIdentity*id,const void*data,size_t n){
 if(r->published.producer_closed||r->next_order==UINT64_MAX){if(r->next_order==UINT64_MAX)r->producer.exhausted=true;return false;}
 unsigned k=kind==PUMP_INGRESS_LOSS?0:1;while(k<CAPTURE_ROUTE_SLOTS&&r->slots[k].state!=FREE)k++;
 if(k==CAPTURE_ROUTE_SLOTS)return false;
 CaptureRouteSlot*s=&r->slots[k];s->identity=*id;s->kind=kind;s->order=r->next_order++;s->bytes=n;memcpy(&s->payload,data,n);s->state=QUEUED;return true;
}
static void lost(CaptureStorageRoute*r,const StorageIngressLoss*m){
 inc(&r->producer.dropped);
 if(r->loss_count==CAPTURE_ROUTE_LOSSES){inc(&r->loss_overflow);inc(&r->producer.overflow);return;}
 r->losses[(r->loss_head+r->loss_count)%CAPTURE_ROUTE_LOSSES]=*m;r->loss_count++;
}
void capture_storage_route_flush(CaptureStorageRoute*r){
 if(!attached(r)||!lock(r))return;
 if(r->loss_count){StorageIngressLoss*m=&r->losses[r->loss_head];if(push(r,PUMP_INGRESS_LOSS,&m->identity,m,sizeof(*m))){r->loss_head=(r->loss_head+1)%CAPTURE_ROUTE_LOSSES;r->loss_count--;}}
 else if(r->loss_overflow){StorageIngressLoss m={0};m.identity.session_id=r->session_id;m.reason=STORAGE_INGRESS_OVERFLOW;m.count=r->loss_overflow;if(push(r,PUMP_INGRESS_LOSS,&m.identity,&m,sizeof(m)))r->loss_overflow=0;}
 publish(r);unlock(r);
}
bool capture_storage_route_packet(CaptureStorageRoute*r,const LfnServiceIdentity*id,const PacketView*p){
 if(!attached(r)||!id||!p||id->session_id!=r->session_id||p->header.capture_id!=id->capture_id||p->header.config_id!=id->config.config_id||(p->header.type!=LFN_PCM&&p->header.type!=LFN_RAW)||p->payload_bytes+LFN_HEADER_BYTES>LFN_MAX_PACKET||!p->payload){if(r)inc(&r->producer.invalid);return false;}
 inc(&r->producer.offered);StorageIngressLoss m={*id,p->header.type,0,p->header.first_raw_sample_index,p->end_raw,p->header.stream_seq,1};
 if(!lock(r)){m.reason=STORAGE_INGRESS_LOCK;lost(r,&m);return false;}
 bool ok=push(r,PUMP_PCM,id,p->payload-LFN_HEADER_BYTES,p->payload_bytes+LFN_HEADER_BYTES);
 if(ok)inc(&r->producer.copied);else{m.reason=STORAGE_INGRESS_FULL;lost(r,&m);}publish(r);unlock(r);return ok;
}
bool capture_storage_route_psd(CaptureStorageRoute*r,const LfnServiceIdentity*identity,const LfnSpectrum*s){
 if(!attached(r)||!identity||!s||!s->valid||s->session_id!=r->session_id||identity->session_id!=s->session_id||identity->capture_id!=s->capture_id||identity->config.config_id!=s->config_id){if(r)inc(&r->producer.invalid);return false;}
 if(r->have_psd&&r->psd_session==s->session_id&&r->psd_capture==s->capture_id&&r->psd_config==s->config_id&&r->psd_seq==s->stream_seq)return true;
 r->have_psd=true;r->psd_session=s->session_id;r->psd_capture=s->capture_id;r->psd_config=s->config_id;r->psd_seq=s->stream_seq;
 /* Source/config comes from the explicit producer-owned identity;
  * reject mixed snapshots instead of relabelling spectrum using newest config. */
 LfnServiceIdentity id=*identity;
 if(id.session_id!=s->session_id||id.capture_id!=s->capture_id||id.config.config_id!=s->config_id){inc(&r->producer.invalid);return false;}
 inc(&r->producer.offered);StorageIngressLoss m={id,LFN_FFT,0,s->first_raw,s->end_raw,s->stream_seq,1};
 if(!lock(r)){m.reason=STORAGE_INGRESS_LOCK;lost(r,&m);return false;}
 bool ok=push(r,PUMP_PSD,&id,s,sizeof(*s));if(ok)inc(&r->producer.copied);else{m.reason=STORAGE_INGRESS_FULL;lost(r,&m);}publish(r);unlock(r);return ok;
}
bool capture_storage_route_producer_close(CaptureStorageRoute*r){
 if(!attached(r))return false;
 capture_storage_route_flush(r);if(r->loss_count||r->loss_overflow||!lock(r))return false;
 r->published.producer_closed=true;publish(r);unlock(r);return true;
}
bool capture_storage_route_facts_publish(CaptureStorageRoute*r,const CaptureRouteFacts*f){
 if(!attached(r)||!f||!lock(r))return false;
 bool ok=r->facts_version!=UINT64_MAX&&(!f->generation||f->generation==r->published.generation);
 if(ok){r->facts=*f;r->facts.generation=r->published.generation;r->facts.version=++r->facts_version;}
 else if(r->facts_version==UINT64_MAX)r->producer.exhausted=true;
 publish(r);unlock(r);return ok;
}
bool capture_storage_route_facts_copy(CaptureStorageRoute*r,CaptureRouteFacts*out){if(!attached(r)||!out||!lock(r))return false;bool ok=r->facts.version!=0;if(ok)*out=r->facts;unlock(r);return ok;}
bool capture_storage_route_health(CaptureStorageRoute*r,CaptureRouteHealth*out){if(!attached(r)||!out||!lock(r))return false;*out=r->published;unlock(r);return true;}
static void consumer_publish(CaptureStorageRoute*r){
 if(r->consumer.blocked&&!r->consumer.error_kind)
  r->consumer.error_kind=r->active==ENERGY?PUMP_ENERGY:r->active<CAPTURE_ROUTE_SLOTS?r->slots[r->active].kind:0;
 if(lock(r)){r->published.consumer=r->consumer;unlock(r);}
}
static bool choose_slot(CaptureStorageRoute*r){
 if(!lock(r))return false;
 uint64_t order=UINT64_MAX;unsigned at=NONE;for(unsigned k=0;k<CAPTURE_ROUTE_SLOTS;k++)if(r->slots[k].state==QUEUED&&r->slots[k].order<order){order=r->slots[k].order;at=k;}
 if(at!=NONE){r->active=at;r->slots[at].state=PINNED;}unlock(r);return at!=NONE;
}
static bool choose_energy(CaptureStorageRoute*r){
 if(lfn_service_energy_take_owned(r->bridge,r,&r->energy)!=SERVICE_OK)return false;
 r->energy_pending=true;r->active=ENERGY;return true;
}
static bool release_active(CaptureStorageRoute*r){
 if(r->active==ENERGY){r->energy_pending=false;r->active=NONE;return true;}
 if(!lock(r))return false;
 r->slots[r->active].state=FREE;unlock(r);r->active=NONE;return true;
}
void capture_storage_route_step(CaptureStorageRoute*r){
 if(!attached(r))return;
 if(r->consumer.blocked){consumer_publish(r);return;}
 if(r->has_ticket){
  if(!r->result_ready){StorageResult p=r->sink.poll(r->sink.context,r->ticket,&r->result);if(p==STORE_BUSY)return;if(p!=STORE_OK){r->consumer.blocked=true;r->consumer.error=p;consumer_publish(r);return;}if(!r->result.done)return;r->result_ready=true;}
  StorageResult a=r->sink.ack(r->sink.context,r->ticket);if(a==STORE_BUSY)return;if(a!=STORE_OK){r->consumer.blocked=true;r->consumer.error=a;consumer_publish(r);return;}
  r->has_ticket=false;r->result_ready=false;
  if(r->result.last_submit.result!=STORE_OK||r->result.aborted){
   r->consumer.error=r->result.last_submit.result;inc(&r->consumer.rejected);
   if(r->consumer.error==STORE_DEPENDENCY||r->consumer.error==STORE_BUSY||r->consumer.error==STORE_CAPACITY){inc(&r->consumer.retries);consumer_publish(r);return;}
   r->consumer.blocked=true;consumer_publish(r);return;
  }
  /* Keep result while source release lock is busy, without ACKing twice. */
  r->result_ready=true;
 }
 if(r->result_ready){
  bool loss=r->active<CAPTURE_ROUTE_SLOTS&&r->slots[r->active].kind==PUMP_INGRESS_LOSS;
  if(!release_active(r))return;
  if(loss)inc(&r->consumer.loss_accepted);
  inc(&r->consumer.accepted);r->consumer.error=STORE_OK;r->result_ready=false;
 }
 if(r->active==NONE){
  bool chosen=r->prefer_energy?(choose_energy(r)||choose_slot(r)):(choose_slot(r)||choose_energy(r));
  if(!chosen){
   bool close=false;if(lock(r)){close=r->published.producer_closed;for(unsigned k=0;k<CAPTURE_ROUTE_SLOTS;k++)if(r->slots[k].state!=FREE)close=false;unlock(r);}
   if(close){LfnServiceMessage probe;LfnServiceResult e=lfn_service_energy_take_owned(r->bridge,r,&probe);if(e==SERVICE_OK){r->energy=probe;r->energy_pending=true;r->active=ENERGY;}else if(e==SERVICE_EMPTY){StorageResult closed=r->sink.close(r->sink.context);if(closed==STORE_OK)r->consumer.handoff_drained=true;else if(closed!=STORE_BUSY&&closed!=STORE_CAPACITY){r->consumer.blocked=true;r->consumer.error=closed;}}}
   consumer_publish(r);return;
  }
  r->prefer_energy=!r->prefer_energy;
 }
 StorageResult result;
 if(r->active==ENERGY)result=r->sink.energy(r->sink.context,&r->energy,&r->ticket);
 else{CaptureRouteSlot*s=&r->slots[r->active];
  if(s->kind==PUMP_PCM)result=r->sink.pcm(r->sink.context,s->identity.session_id,s->payload.bytes,s->bytes,&r->ticket);
  else if(s->kind==PUMP_PSD)result=r->sink.psd(r->sink.context,&s->payload.spectrum,&r->ticket);
  else result=r->sink.loss(r->sink.context,&s->payload.loss,&r->ticket);
 }
 if(result==STORE_OK){r->has_ticket=true;r->result_ready=false;}
 else if(result==STORE_BUSY||result==STORE_CAPACITY||result==STORE_DEPENDENCY)inc(&r->consumer.retries);
 else{r->consumer.blocked=true;r->consumer.error=result;}
 consumer_publish(r);
}

StorageResult capture_storage_route_reconnect(CaptureStorageRoute*r,const CaptureStorageSink*s,uint64_t expected){
 if(!attached(r)||!s||!s->energy||!s->psd||!s->pcm||!s->loss||!s->poll||!s->ack||!s->close||!expected)return STORE_INVALID;
 if(!lock(r))return STORE_BUSY;
 StorageResult result=STORE_BUSY;
 if(r->published.generation!=expected)result=STORE_EXPIRED;
 else if(expected==UINT64_MAX||r->facts_version==UINT64_MAX)result=STORE_EXHAUSTED;
 else if(r->published.producer_closed&&r->consumer.handoff_drained&&!r->consumer.blocked&&!r->has_ticket&&!r->result_ready&&!r->energy_pending&&r->active==NONE&&!r->loss_count&&!r->loss_overflow){
  bool free=true;for(unsigned k=0;k<CAPTURE_ROUTE_SLOTS;k++)if(r->slots[k].state!=FREE)free=false;
  /* Do not nest route and C7 queue locks; owners are externally paused. */
  unlock(r);if(!free)return STORE_BUSY;
  if(lfn_service_consumer_empty(r->bridge,r)!=SERVICE_EMPTY)return STORE_BUSY;
  if(!lock(r))return STORE_BUSY;
  r->sink=*s;r->published.producer_closed=false;r->consumer.handoff_drained=false;r->published.consumer=r->consumer;r->published.generation++;
  memset(&r->facts,0,sizeof(r->facts));r->facts.version=++r->facts_version;result=STORE_OK;
 }
 unlock(r);return result;
}
StorageResult capture_storage_route_unbind(CaptureStorageRoute*r,uint64_t expected){
 if(!r||!r->bridge||!expected||!attached(r))return STORE_INVALID;
 if(!lock(r))return STORE_BUSY;
 StorageResult result=STORE_BUSY;
 if(r->published.generation!=expected)result=STORE_EXPIRED;
 else if(r->published.producer_closed&&r->consumer.handoff_drained&&!r->consumer.blocked&&
         !r->has_ticket&&!r->result_ready&&!r->energy_pending&&r->active==NONE&&
         !r->loss_count&&!r->loss_overflow){
  bool free=true;for(unsigned k=0;k<CAPTURE_ROUTE_SLOTS;k++)if(r->slots[k].state!=FREE){free=false;break;}
  if(free){
   LfnServiceBridge *bridge=r->bridge;
   /* Owners are stopped; do not nest the route lock with the C7 queue lock. */
   unlock(r);
   LfnServiceResult released=lfn_service_consumer_release(bridge,r);
   if(released==SERVICE_OK){
    r->published.attached=false;atomic_store_explicit(&r->attached,false,memory_order_release);
    r->bridge=NULL;return STORE_OK;
   }
   return released==SERVICE_BUSY?STORE_BUSY:STORE_INVALID;
  }
 }
 unlock(r);return result;
}
