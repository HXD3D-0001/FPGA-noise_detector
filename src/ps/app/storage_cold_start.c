#include "storage_cold_start.h"
#include <string.h>
static bool owned(StorageColdStart *s){return s&&s->epoch&&s->epoch->factory->config.is_owner&&s->epoch->factory->config.is_owner(s->epoch->factory->config.owner_context);}
static bool away(StorageColdStart *s,const void *p,size_t n){
 uintptr_t a=(uintptr_t)s,b=(uintptr_t)p;return p&&n&&a<=UINTPTR_MAX-sizeof(*s)&&b<=UINTPTR_MAX-n&&(a+sizeof(*s)<=b||b+n<=a);
}
static void fail(StorageColdStart *s,StorageResult r){s->failed_at=s->state;s->state=COLD_FAILED;s->error=r;}
bool storage_cold_start_init(StorageColdStart *s,StorageCaptureEpoch *e,CaptureStorageRoute *r,StorageMultiSink *sink,const StorageColdCapturePort *port,const StorageSessionFacts *facts){
 if(!s||!e||!e->factory||!e->lifecycle||!e->segment||!r||!sink||!port||!port->attach||!port->copy||!port->ack||!facts)return false;
 StorageFactoryConfig *c=&e->factory->config;
 if(!c->is_owner||!c->is_owner(c->owner_context)||e->state!=CAPTURE_EPOCH_PREPARE||!e->producer_stopped(e->proof_context)||e->factory->state!=FACTORY_READY||!c->await_first_capture||c->engine->state!=ENGINE_AWAIT_CAPTURE||e->lifecycle->state!=LIFECYCLE_METADATA||e->lifecycle->factory!=e->factory||e->lifecycle->space!=e->space||facts->session_id!=c->session_id||r->session_id!=c->session_id||atomic_load(&r->attached)||r->bridge||r->published.generation!=1||sink->session_id!=c->session_id||sink->active||sink->closing||!storage_multi_sink_matches(sink,&r->sink)||e->segment->state!=SEGMENT_IDLE||e->segment->count!=c->count||e->segment->session_id!=c->session_id)return false;
#define AWAY(p) if(!away(s,(p),sizeof(*(p))))return false
 AWAY(e);AWAY(e->factory);AWAY(e->lifecycle);AWAY(e->space);AWAY(e->handles);AWAY(e->segment);AWAY(r);AWAY(sink);AWAY(c->pool);AWAY(c->family);AWAY(c->owner);AWAY(c->catalog);AWAY(c->verifier);AWAY(c->epoch);AWAY(c->engine);
 if(!away(s,c->arena,STORAGE_POOL_BYTES))return false;
 for(unsigned k=0;k<c->count;k++){
  StorageFactoryFile *f=&c->files[k];StorageQueueHealth h;
  AWAY(f->binding);AWAY(f->writer);AWAY(f->queue);AWAY(f->service);AWAY(f->pump);AWAY(f->plan);
  if(e->segment->services[k]!=f->service||f->config_id||f->capture_id||f->service->health.fatal||f->service->health.sealed||atomic_load(&f->service->registry->published)||storage_queue_health(f->queue,&h)!=STORE_OK||h.accepted||h.live_jobs||h.retained_receipts)return false;
 }
#undef AWAY
 StorageColdCapturePort copy=*port;memset(s,0,sizeof(*s));s->epoch=e;s->route=r;s->sink=sink;s->capture=copy;
 StorageRequest request;if(!storage_build_session(&s->builder,facts,&request,s->session.payload,sizeof(s->session.payload))){fail(s,STORE_INVALID);return false;}
 s->session.identity=request.identity;s->session.bytes=request.payload_bytes;return true;
}
static bool observed(StorageService *v,StorageTicket t){for(unsigned k=0;k<STORAGE_RECEIPTS;k++){StorageServiceSlot *x=&v->slots[k];if(x->occupied&&x->definition&&x->ticket.session_id==t.session_id&&x->ticket.generation==t.generation&&x->ticket.slot==t.slot)return x->observed&&x->state==STORE_SYNCED;}return false;}
StorageResult storage_cold_start_segment(StorageColdStart *s,const StorageConfigFacts *cfg,const StorageCaptureFacts *clock,uint64_t *token){
 if(!owned(s)||!cfg||!clock||!token)return STORE_INVALID;
 if(s->state!=COLD_WAIT_FACTS)return STORE_BUSY;
 CaptureRouteHealth health;if(!capture_storage_route_health(s->route,&health))return STORE_BUSY;
 if(health.generation!=1||health.consumer.blocked)return STORE_EXPIRED;
 CaptureRouteFacts facts;if(!capture_storage_route_facts_copy(s->route,&facts)||!facts.capture_valid||!facts.started_valid||!facts.active_valid)return STORE_BUSY;
 StorageResult result=storage_segment_begin(s->epoch->segment,&facts,cfg,clock,&s->segment_token);
 if(result==STORE_OK){*token=s->segment_token;s->identity=facts.identity;s->state=COLD_SEGMENT;}return result;
}
static bool same_capture(StorageColdStart *s){
 CaptureRouteHealth health;if(!capture_storage_route_health(s->route,&health))return false;
 if(health.generation!=1||health.consumer.blocked){fail(s,STORE_EXPIRED);return false;}
 CaptureRouteFacts f;if(!capture_storage_route_facts_copy(s->route,&f))return false;
 PlCaptureConfig *a=&f.identity.config,*b=&s->identity.config;RecordIdentity *id=&s->epoch->segment->definitions[1].identity;
 if(a->mode!=b->mode||a->source!=b->source||a->channel!=b->channel||a->pattern!=b->pattern||!f.capture_valid||!f.started_valid||!f.active_valid||f.identity.session_id!=id->session_id||f.identity.capture_id!=id->capture_id||f.identity.config.config_id!=id->config_id){fail(s,STORE_EXPIRED);return false;}return true;
}
static void metadata_step(StorageColdStart *s,uint64_t now){storage_lifecycle_step(s->epoch->lifecycle,now);if(s->epoch->lifecycle->state==LIFECYCLE_FAILED)fail(s,STORE_INVALID);}
void storage_cold_start_step(StorageColdStart *s,uint64_t now){
 if(!owned(s)||s->state==COLD_DONE||s->state==COLD_FAILED)return;
 if(s->have_time&&now<s->last_now){fail(s,STORE_INVALID);return;}s->have_time=true;s->last_now=now;
 StorageCaptureEpoch *e=s->epoch;StorageFactoryConfig *c=&e->factory->config;StorageResult result;
 if(s->state==COLD_SESSION_SUBMIT){
  StorageSubmit submit=storage_service_metadata(c->files[s->file].service,&s->session.identity,s->session.payload,s->session.bytes);
  if(submit.result==STORE_OK){s->session_ticket=submit.ticket;s->state=COLD_SESSION_WAIT;}else if(submit.result!=STORE_BUSY&&submit.result!=STORE_CAPACITY){fail(s,submit.result);return;}
  metadata_step(s,now);return;
 }
 if(s->state==COLD_SESSION_WAIT){
  StorageService *v=c->files[s->file].service;StorageReceipt receipt;result=storage_queue_poll(v->queue,s->session_ticket,&receipt);
  if(result!=STORE_OK&&result!=STORE_BUSY){fail(s,result);return;}
  if(result==STORE_OK&&receipt.state==STORE_FAILED){fail(s,STORE_INVALID);return;}
  if(result==STORE_OK&&receipt.state==STORE_SYNCED&&observed(v,s->session_ticket)){s->state=++s->file==c->count?COLD_ATTACH_SUBMIT:COLD_SESSION_SUBMIT;}
  metadata_step(s,now);return;
 }
 if(s->state==COLD_ATTACH_SUBMIT){result=s->capture.attach(s->capture.context,s->route,&s->ticket);if(result==STORE_OK)s->state=COLD_ATTACH_WAIT;else if(result!=STORE_BUSY)fail(s,result);return;}
 if(s->state==COLD_ATTACH_WAIT){
  result=s->capture.copy(s->capture.context,s->ticket,&s->receipt);if(result==STORE_BUSY)return;
  if(result!=STORE_OK){fail(s,result);return;}
  if(s->receipt.ticket!=s->ticket||s->receipt.kind!=CONTROL_ATTACH||s->receipt.result!=STORE_OK||s->receipt.barrier.paused||s->receipt.barrier.pause_token||s->receipt.barrier.route_generation!=1){fail(s,STORE_INVALID);return;}
  s->state=COLD_STAGE;return;
 }
 if(s->state==COLD_STAGE){result=storage_capture_epoch_stage_first(e,s->route,s->sink);if(result==STORE_OK)s->state=COLD_ATTACH_ACK;else if(result!=STORE_BUSY)fail(s,result);return;}
 if(s->state==COLD_ATTACH_ACK){result=s->capture.ack(s->capture.context,s->ticket);if(result==STORE_OK)s->state=COLD_WAIT_FACTS;else if(result!=STORE_BUSY)fail(s,result);return;}
 if(s->state==COLD_WAIT_FACTS)return;
 if(s->state>=COLD_SEGMENT&&s->state<=COLD_ACTIVATE&&!same_capture(s))return;
 if(s->state==COLD_SEGMENT){storage_segment_step(e->segment);if(e->segment->state==SEGMENT_FAILED){fail(s,e->segment->error);return;}if(e->segment->state==SEGMENT_SYNCED)s->state=COLD_BIND;metadata_step(s,now);return;}
 RecordIdentity *id=&e->segment->definitions[1].identity;
 if(s->state==COLD_BIND){result=storage_factory_bind_first_capture(e->factory,id->config_id,id->capture_id);if(result==STORE_OK)s->state=COLD_ENABLE;else if(result!=STORE_BUSY)fail(s,result);return;}
 if(s->state==COLD_ENABLE){result=storage_lifecycle_enable(e->lifecycle);if(result==STORE_OK)s->state=COLD_ACTIVATE;else if(result!=STORE_BUSY)fail(s,result);return;}
 if(s->state==COLD_ACTIVATE){result=storage_capture_epoch_activate_first(e);if(result==STORE_OK)s->state=COLD_SEGMENT_ACK;else if(result!=STORE_BUSY)fail(s,result);return;}
 if(s->state==COLD_SEGMENT_ACK){result=storage_segment_ack(e->segment,s->segment_token);if(result==STORE_OK)s->state=COLD_DONE;else fail(s,result);}
}
