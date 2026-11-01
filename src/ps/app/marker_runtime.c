#include "marker_runtime.h"
#include "../storage/marker_record.h"
#include <string.h>
static bool lock(MarkerRuntime*r){return !atomic_flag_test_and_set_explicit(&r->lock,memory_order_acquire);}
static void unlock(MarkerRuntime*r){atomic_flag_clear_explicit(&r->lock,memory_order_release);}
static bool same_ticket(StorageTicket a,StorageTicket b){return a.session_id==b.session_id&&a.generation==b.generation&&a.slot==b.slot;}
static bool same_identity(const RecordIdentity*a,const RecordIdentity*b){return a->type==b->type&&a->session_id==b->session_id&&a->capture_id==b->capture_id&&a->config_id==b->config_id&&a->first_raw==b->first_raw&&a->end_raw==b->end_raw&&a->record_seq==b->record_seq;}
static void fault(MarkerRuntime*r,StorageResult e){r->error=e;atomic_store(&r->health_error,(unsigned)e);}
void marker_runtime_init(MarkerRuntime*r,uint64_t session,SummaryRuntime*s){memset(r,0,sizeof(*r));atomic_flag_clear(&r->lock);r->session=session;r->summary=s;}
void marker_runtime_enable(MarkerRuntime*r,uint64_t capture,uint32_t config){
 if(!r||!capture||!config||!lock(r))return;
 if(!r->closing){r->capture=capture;r->config=config;r->accepting=true;}unlock(r);
}
bool marker_runtime_enqueue(MarkerRuntime*r,const LfnMarker*m){
 if(!r||!m||!m->marker_id||!lock(r))return false;
 bool ok=r->accepting&&!r->closing&&r->count<MARKER_RUNTIME_QUEUE&&m->start.session_id==r->session&&m->start.capture_id==r->capture&&m->start.config_id==r->config;
 if(ok){r->queue[(r->head+r->count)%MARKER_RUNTIME_QUEUE]=*m;r->count++;atomic_fetch_add(&r->queued,1);atomic_fetch_add(&r->pending,1);atomic_store(&r->last_id,m->marker_id);}unlock(r);return ok;
}
static bool admission(void*ctx,const StorageServiceSlot*s,const StorageRequest*q){
 MarkerRuntime*r=ctx;
 if(s->identity.type!=RECORD_MARKER)return !r->service_delegate.admission||r->service_delegate.admission(r->service_delegate.context,s,q);
 if(!r->submitting||!r->active||r->submitted||s->definition){fault(r,STORE_INVALID);return false;}
 r->ticket=s->ticket;r->identity=s->identity;r->file_id=r->service->queue->file_id;r->submitted=true;return true;
}
static bool terminal(void*ctx,const StorageServiceSlot*s){
 MarkerRuntime*r=ctx;
 if(s->identity.type!=RECORD_MARKER)return !r->service_delegate.terminal||r->service_delegate.terminal(r->service_delegate.context,s);
 if(!r->active||!r->submitted||!same_ticket(r->ticket,s->ticket)||!same_identity(&r->identity,&s->identity)||r->file_id!=r->service->queue->file_id){fault(r,STORE_INVALID);return false;}
 r->terminal_state=s->state;r->terminal_seen=true;return true;
}
static StorageResult preflight(void*c,const StorageMultiSource*s){MarkerRuntime*r=c;return r->delegate.preflight?r->delegate.preflight(r->delegate.context,s):STORE_OK;}
static bool commit(void*c,const StorageMultiSource*s,StoragePump*p,StoragePumpTicket t){MarkerRuntime*r=c;return !r->delegate.commit||r->delegate.commit(r->delegate.context,s,p,t);}
static void cancel(void*c){MarkerRuntime*r=c;if(r->delegate.cancel)r->delegate.cancel(r->delegate.context);}
static StorageResult before_close(void*c){
 MarkerRuntime*r=c;if(r->error)return r->error;
 if(!lock(r))return STORE_BUSY;
 r->accepting=false;r->closing=true;unlock(r);
 marker_runtime_step(r,r->now_us);
 if(r->error)return r->error;
 if(atomic_load(&r->pending))return STORE_BUSY;
 return r->delegate.before_close?r->delegate.before_close(r->delegate.context):STORE_OK;
}
bool marker_runtime_attach(MarkerRuntime*r,StorageMultiSink*s,StorageService*service){
 if(!r||!s||!service||atomic_load(&r->pending)||r->error||!lock(r))return false;
 r->service=service;r->service_delegate=service->observer;r->delegate=s->observer;r->accepting=false;r->closing=false;unlock(r);
 service->observer=(StorageServiceObserver){r,admission,terminal};s->observer=(StorageMultiObserver){r,preflight,commit,cancel,before_close};return true;
}
void marker_runtime_step(MarkerRuntime*r,uint64_t now){
 if(!r||r->error||!r->service)return;
 r->now_us=now;
 if(r->terminal_seen){
  if(r->terminal_state==STORE_SYNCED){atomic_fetch_add(&r->synced,1);summary_runtime_marker_synced(r->summary,&r->current);}
  else {atomic_fetch_add(&r->failed,1);atomic_store(&r->health_error,STORE_INVALID);}
  r->active=r->submitted=r->terminal_seen=false;atomic_fetch_sub(&r->pending,1);
 }
 if(!r->active){
  if(!lock(r))return;
  if(r->count){r->current=r->queue[r->head];r->head=(r->head+1)%MARKER_RUNTIME_QUEUE;r->count--;r->active=true;r->started_us=now;}
  unlock(r);if(!r->active)return;
 }
 if(now-r->started_us>=UINT64_C(5000000)){
  if(r->submitted){fault(r,STORE_EXPIRED);return;}
  atomic_fetch_add(&r->failed,1);atomic_fetch_sub(&r->pending,1);atomic_store(&r->health_error,STORE_EXPIRED);r->active=false;return;
 }
 if(!r->submitted){
  r->submitting=true;StorageSubmit s=storage_service_marker(r->service,&r->current);r->submitting=false;
  if(s.result==STORE_OK){if(!r->submitted||!same_ticket(s.ticket,r->ticket))fault(r,STORE_INVALID);}
  else if(s.result!=STORE_BUSY&&s.result!=STORE_CAPACITY&&s.result!=STORE_DEPENDENCY){atomic_fetch_add(&r->failed,1);atomic_fetch_sub(&r->pending,1);atomic_store(&r->health_error,s.result);r->active=false;}
 }
 if(r->submitted)storage_writer_request_sync(r->service->queue->writer);
}
void marker_runtime_health(const MarkerRuntime*r,MarkerRuntimeHealth*h){
 memset(h,0,sizeof(*h));h->queued=atomic_load(&r->queued);h->synced=atomic_load(&r->synced);h->failed=atomic_load(&r->failed);h->last_id=atomic_load(&r->last_id);h->pending=atomic_load(&r->pending);h->error=atomic_load(&r->health_error);
}
