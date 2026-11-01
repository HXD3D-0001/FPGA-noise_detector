#include "capture_request.h"
#include <string.h>
static bool lock(CaptureRequest *r){return !r->locks.lock||r->locks.lock(r->locks.context);}
static void unlock(CaptureRequest *r){if(r->locks.unlock)r->locks.unlock(r->locks.context);}
bool capture_request_init(CaptureRequest *r,CaptureApplication *a,const StorageQueueLocks *locks,uint64_t seed,bool (*owner)(void *),bool (*admit)(void *),void *ctx){
 if(!r||!a||!locks||!!locks->lock!=!!locks->unlock||!seed||seed==UINT64_MAX||!owner)return false;
 StorageQueueLocks saved=*locks;memset(r,0,sizeof(*r));r->application=a;r->locks=saved;r->next_ticket=seed;r->is_capture_owner=owner;r->admit=admit;r->context=ctx;return true;
}
StorageResult capture_request_submit(CaptureRequest *r,const CaptureRequestCommand *c,bool retain,uint64_t *ticket){
 if(!r||!c||!ticket||c->kind<REQUEST_START||c->kind>REQUEST_CLOSE||(c->kind==REQUEST_START&&!c->session)||(c->kind==REQUEST_ATTACH&&!c->route))return STORE_INVALID;
 if(!lock(r))return STORE_BUSY;
 StorageResult result=STORE_BUSY;
 if(r->state==REQUEST_IDLE&&(!r->admit||r->admit(r->context))){
  if(r->next_ticket==UINT64_MAX)result=STORE_EXHAUSTED;
  else{r->command=*c;r->retain_receipt=retain;r->ticket=r->next_ticket++;r->state=REQUEST_QUEUED;*ticket=r->ticket;result=STORE_OK;}
 }
 unlock(r);return result;
}
StorageResult capture_request_copy(CaptureRequest *r,uint64_t ticket,CaptureRequestReceipt *out){
 if(!r||!ticket||!out)return STORE_INVALID;
 if(!lock(r))return STORE_BUSY;
 StorageResult result=ticket!=r->ticket?STORE_EXPIRED:r->state==REQUEST_DONE?STORE_OK:STORE_BUSY;
 if(result==STORE_OK)*out=r->receipt;
 unlock(r);return result;
}
StorageResult capture_request_ack(CaptureRequest *r,uint64_t ticket){
 if(!r||!ticket)return STORE_INVALID;
 if(!lock(r))return STORE_BUSY;
 StorageResult result=ticket!=r->ticket?STORE_EXPIRED:r->state!=REQUEST_DONE?STORE_BUSY:STORE_OK;
 if(result==STORE_OK){r->state=REQUEST_IDLE;r->ticket=0;r->command.route=NULL;}
 unlock(r);return result;
}
void capture_request_step(CaptureRequest *r){
 if(!r||!r->is_capture_owner(r->context))return;
 if(!r->pending_result){
  if(!lock(r))return;
  bool execute=r->state==REQUEST_QUEUED;if(execute)r->state=REQUEST_EXECUTING;unlock(r);if(!execute)return;
  CaptureApplication *a=r->application;CaptureRequestCommand *c=&r->command;bool ok;
  switch(c->kind){
   case REQUEST_START:ok=capture_application_request_start(a,c->session,&c->config,&c->limits);break;
   case REQUEST_ATTACH:ok=capture_application_attach_storage(a,c->route);break;
   case REQUEST_CLOSE:ok=capture_application_close_storage(a);break;
   default:
    ok=c->kind==REQUEST_STOP&&a->state==CA_ACTIVE&&a->runtime.state==CR_STOPPED&&a->failure==PL_OK&&a->runtime.failure==PL_OK;
    if(!ok)ok=capture_application_request_stop(a,c->kind==REQUEST_DISCARD);
    break;
  }
  r->pending=(CaptureRequestReceipt){r->ticket,c->kind,ok?STORE_OK:STORE_INVALID,
   c->kind==REQUEST_START?c->session:a->session,c->kind==REQUEST_START?c->config.config_id:a->selected.config_id,
   c->kind==REQUEST_START?c->config.source:a->selected.source,a->state,a->runtime.state,a->failure?a->failure:a->runtime.failure};
  r->pending_result=true;
 }
 if(!lock(r))return;
 r->receipt=r->pending;if(r->receipt.result!=STORE_OK&&r->rejected!=UINT64_MAX)r->rejected++;
 r->state=r->retain_receipt?REQUEST_DONE:REQUEST_IDLE;
 if(!r->retain_receipt){r->ticket=0;r->command.route=NULL;}
 r->pending_result=false;unlock(r);
}
