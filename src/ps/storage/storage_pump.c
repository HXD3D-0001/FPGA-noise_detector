#include "storage_pump.h"
#include <string.h>
enum { FREE,WAIT,ACTIVE,DONE,NONE=STORAGE_PUMP_SLOTS };
static volatile unsigned pump_stage;
unsigned storage_pump_debug_stage(void){return pump_stage;}
static bool lock(StoragePump*p){return !p->locks.lock||p->locks.lock(p->locks.context);}
static void unlock(StoragePump*p){if(p->locks.unlock)p->locks.unlock(p->locks.context);}
static StoragePumpSlot*slot(StoragePump*p,StoragePumpTicket t){if(t.slot>=STORAGE_PUMP_SLOTS)return NULL;StoragePumpSlot*s=&p->slots[t.slot];return s->state!=FREE&&s->generation==t.generation?s:NULL;}
bool storage_pump_init(StoragePump*p,StorageService*s,const StorageQueueLocks*l){
 if(!p||!s||!s->queue||!l||!!l->lock!=!!l->unlock)return false;
 memset(p,0,sizeof(*p));p->service=s;p->locks=*l;p->active=NONE;p->next_order=1;return true;
}
static StorageResult offer(StoragePump*p,StoragePumpKind kind,uint64_t session,const RecordIdentity*i,const void*data,size_t n,StoragePumpTicket*out){
 if(!p||!p->service||!data||!out||!n||n>sizeof(StoragePumpPayload))return STORE_INVALID;
 if(!lock(p))return STORE_BUSY;
 if(p->health.closed||p->health.fatal){unlock(p);return STORE_BUSY;}
 if(p->next_order==UINT64_MAX){unlock(p);return STORE_EXHAUSTED;}
 unsigned k=(kind==PUMP_METADATA||kind==PUMP_INGRESS_LOSS)?0:STORAGE_PUMP_RESERVED;
 while(k<STORAGE_PUMP_SLOTS&&(p->slots[k].state!=FREE||p->slots[k].generation==UINT64_MAX))k++;
 if(k==STORAGE_PUMP_SLOTS){p->health.rejected++;unlock(p);return STORE_CAPACITY;}
 StoragePumpSlot*s=&p->slots[k];uint64_t generation=s->generation+1;memset(s,0,sizeof(*s));s->generation=generation;s->order=p->next_order++;s->kind=kind;s->session=session;if(i)s->identity=*i;s->bytes=n;memcpy(&s->payload,data,n);s->state=WAIT;
 *out=(StoragePumpTicket){generation,k};p->health.accepted++;p->health.occupied++;unlock(p);return STORE_OK;
}
StorageResult storage_pump_rebind_empty(StoragePump *p,StorageService *s){
 if(!p||!s||!s->queue||s->health.sealed||s->health.fatal)return STORE_INVALID;
 if(!lock(p))return STORE_BUSY;
 if(!p->health.closed||p->health.occupied||p->active!=NONE||p->staged||p->publish_pending||p->admin_active||p->admin_publish){unlock(p);return STORE_BUSY;}
 p->service=s;memset(&p->health,0,sizeof(p->health));unlock(p);return STORE_OK;
}
StorageResult storage_pump_metadata(StoragePump*p,const RecordIdentity*i,const uint8_t*b,size_t n,StoragePumpTicket*t){if(!i||i->record_seq||n>STORAGE_METADATA_BYTES)return STORE_INVALID;return offer(p,PUMP_METADATA,i->session_id,i,b,n,t);}
StorageResult storage_pump_energy(StoragePump*p,const LfnServiceMessage*m,StoragePumpTicket*t){return offer(p,PUMP_ENERGY,0,NULL,m,sizeof(*m),t);}
StorageResult storage_pump_ingress_loss(StoragePump*p,const StorageIngressLoss*m,StoragePumpTicket*t){return offer(p,PUMP_INGRESS_LOSS,0,NULL,m,sizeof(*m),t);}
StorageResult storage_pump_summary(StoragePump*p,const MinuteSummary*s,StoragePumpTicket*t){return offer(p,PUMP_SUMMARY,0,NULL,s,sizeof(*s),t);}
StorageResult storage_pump_psd(StoragePump*p,const LfnSpectrum*s,StoragePumpTicket*t){return offer(p,PUMP_PSD,0,NULL,s,sizeof(*s),t);}
StorageResult storage_pump_pcm(StoragePump*p,uint64_t session,const uint8_t*b,size_t n,StoragePumpTicket*t){if(!session||n>LFN_MAX_PACKET)return STORE_INVALID;return offer(p,PUMP_PCM,session,NULL,b,n,t);}
StorageResult storage_pump_poll(StoragePump*p,StoragePumpTicket t,StoragePumpResult*out){
 if(!p||!out)return STORE_INVALID;
 if(!lock(p))return STORE_BUSY;
 StoragePumpSlot*s=slot(p,t);StorageResult r=s?STORE_OK:STORE_EXPIRED;if(s)*out=s->result;unlock(p);return r;
}
StorageResult storage_pump_ack(StoragePump*p,StoragePumpTicket t){
 if(!p)return STORE_INVALID;
 if(!lock(p))return STORE_BUSY;
 StoragePumpSlot*s=slot(p,t);StorageResult r=s?STORE_BUSY:STORE_EXPIRED;if(s&&s->state==DONE){s->state=FREE;p->health.occupied--;r=STORE_OK;}unlock(p);return r;
}
StorageResult storage_pump_abort(StoragePump*p,StoragePumpTicket t){
 if(!p)return STORE_INVALID;
 if(!lock(p))return STORE_BUSY;
 StoragePumpSlot*s=slot(p,t);StorageResult r=s?STORE_BUSY:STORE_EXPIRED;if(s&&s->state!=DONE){s->abort_requested=true;r=STORE_OK;}unlock(p);return r;
}
StorageResult storage_pump_health(StoragePump*p,StoragePumpHealth*out){if(!p||!out)return STORE_INVALID;if(!lock(p))return STORE_BUSY;*out=p->health;unlock(p);return STORE_OK;}
StorageResult storage_pump_close(StoragePump*p){if(!p)return STORE_INVALID;if(!lock(p))return STORE_BUSY;p->health.closed=true;unlock(p);return STORE_OK;}
StorageResult storage_pump_recover(StoragePump*p,bool repair){if(!p)return STORE_INVALID;if(!lock(p))return STORE_BUSY;if(p->admin_active){unlock(p);return STORE_BUSY;}p->admin_active=true;p->admin=repair?2:1;p->health.admin_result=STORE_BUSY;p->health.admin_pending=true;p->health.drained=false;unlock(p);return STORE_OK;}
static bool publish(StoragePump*p){
 if(!p->publish_pending)return true;
 if(!lock(p))return false;
 StoragePumpSlot*s=&p->slots[p->active];s->result=p->completion;
 if(p->completion.done){s->state=DONE;p->health.completed++;p->active=NONE;p->staged=false;}
 p->publish_pending=false;unlock(p);return true;
}
static bool publish_admin(StoragePump*p){
 if(!p->admin_publish)return true;
 if(!lock(p))return false;
 p->health.admin_result=p->admin_completion;p->health.admin_pending=false;p->health.drained=false;p->admin_active=false;p->admin_publish=false;unlock(p);return true;
}
void storage_pump_step(StoragePump*p,uint64_t now){
 if(!p||!p->service)return;
 /* A successful service admission is never repeated if publication lock busy. */
 if(!publish(p)||!publish_admin(p))return;
 storage_service_step(p->service,now);
 if(!lock(p))return;
 p->health.steps++;p->health.drained=p->service->health.drained;p->health.fatal=p->service->health.fatal;
 p->health.synced=p->service->health.synced;p->health.failed=p->service->health.failed;p->health.unsaved_ranges=p->service->unsaved.count;p->health.inflight_ranges=p->service->inflight.count;p->health.unsaved_overflow=p->service->unsaved.overflow;
 p->health.writer_state=p->service->queue->writer->state;p->health.writer_error=p->service->queue->writer->error;
 if(p->active==NONE){uint64_t order=UINT64_MAX;for(unsigned k=0;k<STORAGE_PUMP_SLOTS;k++)if(p->slots[k].state==WAIT&&p->slots[k].order<order){order=p->slots[k].order;p->active=k;}if(p->active!=NONE){p->slots[p->active].state=ACTIVE;p->completion=(StoragePumpResult){0};p->staged=false;}}
 if(p->active==NONE){unsigned admin=p->admin;p->admin=0;bool close=p->health.closed;unlock(p);
  if(admin){bool ok=admin==2?storage_service_repair_tail(p->service):storage_service_recover(p->service);p->admin_completion=ok?STORE_OK:STORE_BUSY;p->admin_publish=true;(void)publish_admin(p);}
  if(close&&!p->service->health.sealed)(void)storage_service_seal(p->service);
  return;
 }
 StoragePumpSlot*s=&p->slots[p->active];bool abort=s->abort_requested;unlock(p);
 if(abort){p->completion.aborted=true;p->completion.done=true;if(!p->completion.pieces)p->completion.last_submit.result=STORE_INVALID;p->completion.have_remainder=p->staged&&storage_service_discard_pcm(p->service,&p->completion.remainder);}
 else{
  StorageSubmit result={.result=STORE_INVALID};
  if(s->kind==PUMP_METADATA)result=storage_service_metadata(p->service,&s->identity,s->payload.bytes,s->bytes);
  else if(s->kind==PUMP_ENERGY)result=storage_service_offer(p->service,&s->payload.energy);
  else if(s->kind==PUMP_INGRESS_LOSS)result=storage_service_ingress_loss(p->service,&s->payload.loss);
  else if(s->kind==PUMP_SUMMARY)result=storage_service_summary(p->service,&s->payload.summary);
  else if(s->kind==PUMP_PSD)result=storage_service_psd(p->service,&s->payload.spectrum);
  else if(s->kind==PUMP_PCM){
   if(!p->staged){result.result=storage_service_stage_pcm(p->service,s->session,s->payload.bytes,s->bytes);if(result.result==STORE_OK)p->staged=true;}
   if(p->staged)result=storage_service_pcm_next(p->service);
  }
  p->completion.last_submit=result;
  if(result.result==STORE_OK){p->completion.pieces++;if(p->completion.pieces==1)p->completion.first_sequence=result.record_seq;p->completion.last_sequence=result.record_seq;p->completion.done=s->kind!=PUMP_PCM||!p->service->pcm_pending;}
  else if(result.result!=STORE_BUSY&&result.result!=STORE_CAPACITY){
   /* A staged remainder remains owned until explicit abort, never forgotten. */
   p->completion.done=!p->staged;
  }
 }
 p->publish_pending=true;(void)publish(p);
}
