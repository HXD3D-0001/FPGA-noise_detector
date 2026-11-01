#include "capture_control.h"
#include <string.h>
static bool lock(CaptureControl *c){return !c->locks.lock||c->locks.lock(c->locks.context);}
static void unlock(CaptureControl *c){if(c->locks.unlock)c->locks.unlock(c->locks.context);}
bool capture_control_init(CaptureControl *c,CaptureApplication *a,const StorageQueueLocks *locks,uint64_t seed,bool (*owner)(void *),bool (*admit)(void *),void *ctx){
 if(!c||!a||!locks||!!locks->lock!=!!locks->unlock||!seed||seed==UINT64_MAX||!owner)return false;
 StorageQueueLocks copy=*locks;memset(c,0,sizeof(*c));c->application=a;c->locks=copy;c->next_ticket=seed;c->is_capture_owner=owner;c->admit=admit;c->context=ctx;return true;
}
static StorageResult submit(CaptureControl *c,CaptureControlKind kind,uint64_t pause,uint64_t generation,CaptureStorageRoute *route,uint64_t *ticket){
 if(!lock(c))return STORE_BUSY;
 StorageResult result=STORE_BUSY;
 if(c->state==CONTROL_IDLE&&(!c->admit||c->admit(c->context))){if(c->next_ticket==UINT64_MAX)result=STORE_EXHAUSTED;else{c->kind=kind;c->stop_phase=0;c->pause_token=pause;c->generation=generation;c->attach_route=route;c->ticket=c->next_ticket++;c->state=CONTROL_QUEUED;*ticket=c->ticket;result=STORE_OK;}}
 unlock(c);return result;
}
StorageResult capture_control_submit(CaptureControl *c,CaptureControlKind kind,uint64_t pause,uint64_t generation,uint64_t *ticket){
 if(!c||!ticket||(kind!=CONTROL_PAUSE&&kind!=CONTROL_RESUME&&kind!=CONTROL_STOP_CLOSE)||((kind==CONTROL_PAUSE||kind==CONTROL_STOP_CLOSE)&&(pause||generation))||(kind==CONTROL_RESUME&&(!pause||!generation)))return STORE_INVALID;
 return submit(c,kind,pause,generation,NULL,ticket);
}
StorageResult capture_control_attach(CaptureControl *c,CaptureStorageRoute *route,uint64_t *ticket){
 if(!c||!route||!ticket)return STORE_INVALID;
 return submit(c,CONTROL_ATTACH,0,0,route,ticket);
}
StorageResult capture_control_copy(CaptureControl *c,uint64_t ticket,CaptureControlReceipt *out){
 if(!c||!ticket||!out)return STORE_INVALID;
 if(!lock(c))return STORE_BUSY;
 StorageResult result=ticket!=c->ticket?STORE_EXPIRED:c->state==CONTROL_DONE?STORE_OK:STORE_BUSY;if(result==STORE_OK)*out=c->receipt;unlock(c);return result;
}
StorageResult capture_control_ack(CaptureControl *c,uint64_t ticket){
 if(!c||!ticket)return STORE_INVALID;
 if(!lock(c))return STORE_BUSY;
 StorageResult result=ticket!=c->ticket?STORE_EXPIRED:c->state!=CONTROL_DONE?STORE_BUSY:STORE_OK;if(result==STORE_OK){c->state=CONTROL_IDLE;c->ticket=0;c->attach_route=NULL;}unlock(c);return result;
}
bool capture_control_barrier(CaptureControl *c,CaptureControlBarrier *out){if(!c||!out||!lock(c))return false;*out=c->barrier;unlock(c);return true;}
static StorageResult apply(CaptureControl *c,CaptureControlBarrier *barrier){
 CaptureApplication *a=c->application;CaptureRouteHealth h;
 if(c->kind==CONTROL_ATTACH){
  if(c->owner_paused||c->barrier.paused)return STORE_INVALID;
  StorageResult result=capture_application_attach_storage_result(a,c->attach_route);
  if(result==STORE_OK)*barrier=(CaptureControlBarrier){false,0,a->storage_generation};
  return result;
 }
 if(c->kind==CONTROL_STOP_CLOSE){
  if(c->owner_paused||c->barrier.paused||a->state!=CA_ACTIVE||!a->bound||!a->prepared||!a->storage||a->clock_pending||a->runtime.failure!=PL_OK||a->failure!=PL_OK||a->runtime.state==CR_FAILED||a->runtime.state==CR_INCOMPATIBLE||a->runtime.state==CR_QUARANTINED)return STORE_INVALID;
  if(!c->stop_phase){
   if(a->runtime.state==CR_RUN){if(!capture_application_request_stop(a,false))return STORE_INVALID;}
   else if(a->runtime.state!=CR_STOPPED&&(a->runtime.state!=CR_CLOSING||a->runtime.continue_start))return STORE_INVALID;
   c->stop_phase=1;
  }
  if(a->runtime.state!=CR_STOPPED)return STORE_BUSY;
  if(!a->runtime.final_clean)return STORE_INVALID;
  if(c->stop_phase==1){
   if(!a->storage_input_closed&&!a->storage_close_requested&&!capture_application_close_storage(a))return STORE_INVALID;
   c->stop_phase=2;
  }
  if(!a->storage_input_closed||a->storage_close_requested)return STORE_BUSY;
  if(!capture_storage_route_health(a->storage,&h))return STORE_BUSY;
  if(!h.attached||!h.producer_closed||h.consumer.blocked||h.generation!=a->storage_generation)return STORE_INVALID;
  *barrier=(CaptureControlBarrier){false,0,h.generation};return STORE_OK;
 }

 if(c->kind==CONTROL_PAUSE){
  if(c->owner_paused||a->state!=CA_ACTIVE||a->runtime.state!=CR_STOPPED||!a->storage_input_closed||a->storage_close_requested||!a->storage)return STORE_INVALID;
  if(!capture_storage_route_health(a->storage,&h))return STORE_BUSY;
  if(!h.attached||!h.producer_closed||!h.consumer.handoff_drained||h.consumer.blocked)return STORE_INVALID;
  *barrier=(CaptureControlBarrier){true,c->ticket,h.generation};c->owner_paused=true;return STORE_OK;
 }
 if(!c->owner_paused||!c->barrier.paused||c->pause_token!=c->barrier.pause_token||c->generation!=c->barrier.route_generation+1)return STORE_INVALID;
 if(!capture_application_resume_storage(a,c->generation))return STORE_INVALID;
 *barrier=(CaptureControlBarrier){false,0,c->generation};return STORE_OK;
}
void capture_control_step(CaptureControl *c){
 if(!c||!c->is_capture_owner(c->context))return;
 if(!c->pending_result){
  if(!lock(c))return;
  bool execute=c->state==CONTROL_QUEUED||c->state==CONTROL_EXECUTING;if(execute)c->state=CONTROL_EXECUTING;unlock(c);if(!execute)return;
  CaptureControlBarrier barrier=c->barrier;StorageResult result=apply(c,&barrier);if(result==STORE_BUSY)return;
  c->pending=(CaptureControlReceipt){c->ticket,c->kind,result,barrier};c->pending_result=true;
 }
 if(!lock(c))return;
 c->receipt=c->pending;c->barrier=c->pending.barrier;c->owner_paused=c->barrier.paused;c->state=CONTROL_DONE;c->pending_result=false;unlock(c);
}
bool capture_control_owner_paused(CaptureControl *c){return !c||!c->is_capture_owner(c->context)||c->owner_paused;}
