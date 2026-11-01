#include "storage_rotation.h"
#include <string.h>
static bool owner(StorageRotation *p){return p&&p->is_owner&&p->is_owner(p->owner_context);}
static void fail(StorageRotation *p,StorageResult error){p->failed_at=p->state;p->error=error;p->state=ROTATION_FAILED;}
bool storage_rotation_stopped(void *v){StorageRotation *p=v;return owner(p)&&p->paused_proof;}
static bool separate(const void *a,size_t an,const void *b,size_t bn){
 if(!a||!b||(uintptr_t)a>UINTPTR_MAX-an||(uintptr_t)b>UINTPTR_MAX-bn)return false;
 return (uintptr_t)a+an<=(uintptr_t)b||(uintptr_t)b+bn<=(uintptr_t)a;
}
/* Enumerate real workspaces without a large temporary region array. */
static bool region(const StorageFactoryConfig *c,StorageFactory *f,unsigned n,const void **ptr,size_t *size){
 const void *base[]={f,c->pool,c->family,c->owner,c->catalog,c->verifier,c->epoch,c->engine,c->arena};
 size_t sizes[]={sizeof(*f),sizeof(*c->pool),sizeof(*c->family),sizeof(*c->owner),sizeof(*c->catalog),sizeof(*c->verifier),sizeof(*c->epoch),sizeof(*c->engine),STORAGE_POOL_BYTES};
 if(n<9){*ptr=base[n];*size=sizes[n];return true;}n-=9;if(n>=6*c->count)return false;
 const StorageFactoryFile *a=&c->files[n/6];
 const void *file[]={a->binding,a->writer,a->queue,a->service,a->pump,a->plan};
 size_t bytes[]={sizeof(*a->binding),sizeof(*a->writer),sizeof(*a->queue),sizeof(*a->service),sizeof(*a->pump),sizeof(*a->plan)};
 *ptr=file[n%6];*size=bytes[n%6];return true;
}
static bool away_factory(const void *ptr,size_t size,const StorageFactoryConfig *c,StorageFactory *f){
 const void *r;size_t n;for(unsigned k=0;region(c,f,k,&r,&n);k++)if(!separate(ptr,size,r,n))return false;return true;
}
bool storage_rotation_init(StorageRotation *p,const StorageRotationCapturePort *c,bool (*own)(void *),void *ctx){
 if(!p||!c||!c->submit||!c->copy||!c->ack||!own||!own(ctx))return false;
 StorageRotationCapturePort copy=*c;memset(p,0,sizeof(*p));p->capture=copy;p->is_owner=own;p->owner_context=ctx;return true;
}
StorageResult storage_rotation_begin(StorageRotation *p,StorageCaptureEpoch *old){
 if(!owner(p)||(p->state!=ROTATION_IDLE&&p->state!=ROTATION_DONE))return STORE_BUSY;
 if(old&&old->count<=STORAGE_FAMILY_FILES&&2*(old->count+2)>STORAGE_HANDLES)return STORE_CAPACITY;
 if(!old||old->state!=CAPTURE_EPOCH_ACTIVE||!old->factory||!old->lifecycle||!old->handles||!old->space||!old->route||!old->route->bridge||!old->sink||!old->count||old->count>STORAGE_FAMILY_FILES||old->factory->config.count!=old->count||old->lifecycle->state!=LIFECYCLE_RUNNING||old->lifecycle->factory!=old->factory||old->lifecycle->space!=old->space||!away_factory(p,sizeof(*p),&old->factory->config,old->factory))return STORE_INVALID;
 const void *ptrs[]={old,old->lifecycle,old->handles,old->space,old->route,old->sink,old->route->bridge};
 size_t sizes[]={sizeof(*old),sizeof(*old->lifecycle),sizeof(*old->handles),sizeof(*old->space),sizeof(*old->route),sizeof(*old->sink),sizeof(*old->route->bridge)};
 for(unsigned k=0;k<7;k++)if(!separate(p,sizeof(*p),ptrs[k],sizes[k]))return STORE_INVALID;
 if(old->segment&&(old->segment->state!=SEGMENT_IDLE||!separate(p,sizeof(*p),old->segment,sizeof(*old->segment))))return STORE_BUSY;
 CaptureRouteHealth h;if(!capture_storage_route_health(old->route,&h))return STORE_BUSY;
 if(!h.attached||h.consumer.blocked||h.generation==UINT64_MAX)return STORE_INVALID;
 p->old=old;p->route=old->route;p->generation=h.generation;p->ticket=p->pause_token=0;p->paused_proof=p->have_time=false;p->error=STORE_OK;memset(&p->next,0,sizeof(p->next));memset(&p->receipt,0,sizeof(p->receipt));p->state=h.producer_closed&&h.consumer.handoff_drained?ROTATION_PAUSE_SUBMIT:ROTATION_CAPTURE_STOP_SUBMIT;return STORE_OK;
}
static bool geometry(StorageRotation *p,const StorageRotationNext *a){
 StorageCaptureEpoch *o=p->old;
 const void *ptrs[]={a->provision,a->epoch,a->lifecycle,a->sink,a->segment};
 size_t sizes[]={sizeof(*a->provision),sizeof(*a->epoch),sizeof(*a->lifecycle),sizeof(*a->sink),sizeof(*a->segment)};
 const void *old[]={p,o,o->lifecycle,o->handles,o->space,p->route,o->sink,o->segment,p->route->bridge};
 size_t os[]={sizeof(*p),sizeof(*o),sizeof(*o->lifecycle),sizeof(*o->handles),sizeof(*o->space),sizeof(*p->route),sizeof(*o->sink),sizeof(*o->segment),sizeof(*p->route->bridge)};
 for(unsigned k=0;k<5;k++){
  if(!away_factory(ptrs[k],sizes[k],&a->config,a->factory)||!away_factory(ptrs[k],sizes[k],&o->factory->config,o->factory))return false;
  for(unsigned j=0;j<k;j++)if(!separate(ptrs[k],sizes[k],ptrs[j],sizes[j]))return false;
  for(unsigned j=0;j<9;j++)if(old[j]&&!separate(ptrs[k],sizes[k],old[j],os[j]))return false;
 }
 const void *r;size_t n;
 for(unsigned k=0;region(&a->config,a->factory,k,&r,&n);k++){
  if(!away_factory(r,n,&o->factory->config,o->factory))return false;
  for(unsigned j=0;j<9;j++)if(old[j]&&!separate(r,n,old[j],os[j]))return false;
 }
 return true;
}
StorageResult storage_rotation_prepare(StorageRotation *p,const StorageRotationNext *a){
 if(!owner(p)||p->state!=ROTATION_NEED_NEXT)return STORE_BUSY;
 StorageFactoryConfig *old=&p->old->factory->config;StorageEpochToken *t=&old->epoch->token;
 if(!a||!separate(p,sizeof(*p),a,sizeof(*a))||!a->provision||!a->epoch||!a->factory||!a->lifecycle||!a->sink||!a->segment||!a->segment_seed||a->segment_seed==UINT64_MAX||!t->valid||t->consumed||old->epoch->state!=EPOCH_READY||t->count!=old->count||a->config.count!=old->count||a->config.session_id!=t->session_id||a->config.next_sequence!=t->next_sequence||!geometry(p,a))return STORE_INVALID;
 for(unsigned k=0;k<4;k++)if(a->targets[k]>=a->config.count)return STORE_INVALID;
 uint64_t ids[STORAGE_FAMILY_FILES]={0},previous[STORAGE_PROVISION_FILES]={t->catalog_id,t->ledger_id};
 for(unsigned k=0;k<t->count;k++)previous[k+2]=t->files[k];
 for(unsigned k=0;k<old->count;k++){
  ids[k]=a->config.files[k].descriptor.file_id;
  if(a->config.files[k].config_id!=old->files[k].config_id||a->config.files[k].capture_id!=old->files[k].capture_id)return STORE_INVALID;
  for(unsigned j=0;j<old->count+2;j++)if(a->slots[k]==p->old->slots[j])return STORE_INVALID;
 }
 for(unsigned k=0;k<a->config.count+2;k++)for(unsigned j=0;j<t->count+2;j++){
  uint64_t id=k<a->config.count?ids[k]:k==a->config.count?a->config.ledger_id:a->config.catalog_id;
  if(id==previous[j]||a->slots[k]==p->old->slots[j])return STORE_INVALID;
 }
 StorageRotationNext copy=*a;StorageEpochHooks hooks;
 if(!storage_capture_epoch_init(copy.epoch,p->old->handles,copy.factory,copy.lifecycle,p->old->space,copy.slots,copy.config.count,storage_rotation_stopped,p,&hooks))return STORE_INVALID;
 copy.config.hooks=hooks;
 if(!storage_provision_init(copy.provision,&copy.ops,p->old->handles,p->old->space,copy.factory,copy.lifecycle,&copy.config,copy.slots))return STORE_INVALID;
 uint64_t session=0,seed=0;
 if(!storage_epoch_consume(old->epoch,copy.config.catalog_id,copy.config.ledger_id,ids,copy.config.count,&session,&seed)||session!=copy.config.session_id||seed!=copy.config.next_sequence){fail(p,STORE_INVALID);return STORE_INVALID;}
 p->next=copy;p->state=ROTATION_CREATE;return STORE_OK;
}
static void command_submit(StorageRotation *p,CaptureControlKind kind){
 StorageResult r=p->capture.submit(p->capture.context,kind,kind==CONTROL_PAUSE?0:p->pause_token,kind==CONTROL_PAUSE?0:p->generation+1,&p->ticket);
 if(r==STORE_BUSY)return;
 if(r!=STORE_OK||!p->ticket){fail(p,r==STORE_OK?STORE_INVALID:r);return;}
 p->state=kind==CONTROL_PAUSE?ROTATION_PAUSE_WAIT:ROTATION_RESUME_WAIT;
}
static void command_copy(StorageRotation *p,bool pause){
 StorageResult r=p->capture.copy(p->capture.context,p->ticket,&p->receipt);if(r==STORE_BUSY)return;
 CaptureControlReceipt *a=&p->receipt;
 if(r!=STORE_OK||a->ticket!=p->ticket||a->kind!=(pause?CONTROL_PAUSE:CONTROL_RESUME)||a->result!=STORE_OK||a->barrier.paused!=pause||a->barrier.route_generation!=p->generation+(pause?0:1)||a->barrier.pause_token!=(pause?p->ticket:0)){fail(p,r!=STORE_OK?r:a->result!=STORE_OK?a->result:STORE_INVALID);return;}
 if(pause){p->pause_token=a->barrier.pause_token;p->paused_proof=true;p->old->producer_stopped=storage_rotation_stopped;p->old->proof_context=p;}
 p->state=pause?ROTATION_PAUSE_ACK:ROTATION_RESUME_ACK;
}
static bool drive_stopping_capture(StorageRotation *p,uint64_t now){
 storage_capture_epoch_step(p->old,now);
 if(p->old->state==CAPTURE_EPOCH_FAILED){fail(p,p->old->error);return false;}
 return true;
}
static void stopping_reply(StorageRotation *p){
 StorageResult r=p->capture.copy(p->capture.context,p->ticket,&p->receipt);if(r==STORE_BUSY)return;
 CaptureControlReceipt *a=&p->receipt;
 if(r!=STORE_OK||a->ticket!=p->ticket||a->kind!=CONTROL_STOP_CLOSE||a->result!=STORE_OK||a->barrier.paused||a->barrier.pause_token||a->barrier.route_generation!=p->generation){fail(p,r!=STORE_OK?r:a->result!=STORE_OK?a->result:STORE_INVALID);return;}
 p->state=ROTATION_CAPTURE_STOP_ACK;
}
void storage_rotation_step(StorageRotation *p,uint64_t now){
 if(!owner(p)||p->state==ROTATION_IDLE||p->state==ROTATION_DONE||p->state==ROTATION_FAILED)return;
 if(p->have_time&&now<p->last_now){fail(p,STORE_INVALID);return;}p->have_time=true;p->last_now=now;
 StorageResult r;StorageRotationNext *a=&p->next;
 if(p->state==ROTATION_CAPTURE_STOP_SUBMIT||p->state==ROTATION_CAPTURE_STOP_WAIT||p->state==ROTATION_CAPTURE_STOP_ACK||p->state==ROTATION_CAPTURE_DRAIN){
  CaptureRouteHealth h;if(!capture_storage_route_health(p->route,&h))return;
  if(!h.attached||h.generation!=p->generation){fail(p,STORE_EXPIRED);return;}
 }
 switch(p->state){
 case ROTATION_CAPTURE_STOP_SUBMIT:
  r=p->capture.submit(p->capture.context,CONTROL_STOP_CLOSE,0,0,&p->ticket);
  if(r==STORE_OK&&p->ticket)p->state=ROTATION_CAPTURE_STOP_WAIT;
  else if(r!=STORE_BUSY){fail(p,r==STORE_OK?STORE_INVALID:r);return;}
  (void)drive_stopping_capture(p,now);return;
 case ROTATION_CAPTURE_STOP_WAIT:
  stopping_reply(p);if(p->state!=ROTATION_FAILED)(void)drive_stopping_capture(p,now);return;
 case ROTATION_CAPTURE_STOP_ACK:
  r=p->capture.ack(p->capture.context,p->ticket);
  if(r==STORE_OK)p->state=ROTATION_CAPTURE_DRAIN;
  else if(r!=STORE_BUSY){fail(p,r);return;}
  (void)drive_stopping_capture(p,now);return;
 case ROTATION_CAPTURE_DRAIN:{
  if(!drive_stopping_capture(p,now))return;
  CaptureRouteHealth h;if(!capture_storage_route_health(p->route,&h))return;
  if(h.generation!=p->generation){fail(p,STORE_EXPIRED);return;}
  if(h.attached&&h.producer_closed&&h.consumer.handoff_drained&&!h.consumer.blocked)p->state=ROTATION_PAUSE_SUBMIT;
  return;}
 case ROTATION_PAUSE_SUBMIT:command_submit(p,CONTROL_PAUSE);return;
 case ROTATION_PAUSE_WAIT:command_copy(p,true);return;
 case ROTATION_PAUSE_ACK:case ROTATION_RESUME_ACK:
  r=p->capture.ack(p->capture.context,p->ticket);if(r==STORE_BUSY)return;if(r!=STORE_OK){fail(p,r);return;}
  p->state=p->state==ROTATION_PAUSE_ACK?ROTATION_STOP_OLD:ROTATION_DONE;return;
 case ROTATION_STOP_OLD:
  r=storage_capture_epoch_stop(p->old);if(r!=STORE_OK){fail(p,r);return;}p->state=ROTATION_CLOSE_OLD;return;
 case ROTATION_CLOSE_OLD:
  storage_capture_epoch_step(p->old,now);if(p->old->state==CAPTURE_EPOCH_FAILED)fail(p,p->old->error);else if(p->old->state==CAPTURE_EPOCH_HANDOFF)p->state=ROTATION_FINISH_OLD;return;
 case ROTATION_FINISH_OLD:
  r=storage_capture_epoch_finish(p->old);if(r==STORE_BUSY)return;if(r!=STORE_OK){fail(p,r);return;}p->state=ROTATION_NEED_NEXT;return;
 case ROTATION_NEED_NEXT:return;
 case ROTATION_CREATE:
  storage_provision_step(a->provision,now);if(a->provision->state==PROVISION_FAILED)fail(p,a->provision->error);else if(a->provision->state==PROVISION_METADATA)p->state=ROTATION_BOOTSTRAP_BEGIN;return;
 case ROTATION_BOOTSTRAP_BEGIN:{
  const StorageBootstrapPlan *plans[STORAGE_FAMILY_FILES];for(unsigned k=0;k<p->old->count;k++)plans[k]=p->old->factory->config.files[k].plan;
  r=storage_lifecycle_bootstrap(a->lifecycle,plans);if(r==STORE_BUSY)return;if(r!=STORE_OK){fail(p,r);return;}p->state=ROTATION_BOOTSTRAP;return;}
 case ROTATION_BOOTSTRAP:
  storage_lifecycle_step(a->lifecycle,now);if(a->lifecycle->state==LIFECYCLE_FAILED)fail(p,STORE_INVALID);else if(a->factory->state==FACTORY_READY)p->state=ROTATION_ENABLE;return;
 case ROTATION_ENABLE:
  r=storage_lifecycle_enable(a->lifecycle);if(r==STORE_BUSY)return;if(r!=STORE_OK){fail(p,r);return;}p->state=ROTATION_CONNECT;return;
 case ROTATION_CONNECT:{
  StoragePump *pumps[4],*closing[STORAGE_FAMILY_FILES];StorageService *services[STORAGE_FAMILY_FILES];CaptureStorageSink view;
  for(unsigned k=0;k<4;k++)pumps[k]=a->factory->config.files[a->targets[k]].pump;
  for(unsigned k=0;k<a->config.count;k++){services[k]=a->factory->config.files[k].service;closing[k]=a->factory->config.files[k].pump;}
  StorageMultiTargets targets={pumps[0],pumps[1],pumps[2],pumps[3]};
  if(!storage_multi_sink_init(a->sink,a->config.session_id,p->old->sink->next_ticket,&targets,&view)||!storage_multi_sink_set_close_pumps(a->sink,closing,a->config.count)||!storage_segment_init(a->segment,services,a->config.count,a->config.session_id,a->segment_seed,p->is_owner,p->owner_context)||storage_capture_epoch_segment(a->epoch,a->segment)!=STORE_OK){fail(p,STORE_INVALID);return;}
  if(a->attach_observers&&!a->attach_observers(a->observer_context,a->sink,p->route,closing,a->config.count,p->generation+1)){fail(p,STORE_INVALID);return;}
  r=storage_capture_epoch_reconnect(p->old,a->epoch,p->route,a->sink);if(r!=STORE_OK){fail(p,r);return;}
  p->paused_proof=false;p->state=ROTATION_RESUME_SUBMIT;return;}
 case ROTATION_RESUME_SUBMIT:command_submit(p,CONTROL_RESUME);return;
 case ROTATION_RESUME_WAIT:command_copy(p,false);return;
 default:fail(p,STORE_INVALID);return;
 }
}
