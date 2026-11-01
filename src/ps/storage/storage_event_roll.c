#include "storage_event_roll.h"
#include <string.h>
static void fail(StorageEventRoll *r,StorageResult error){r->error=error;if(r->factory->config.catalog->phase==CATALOG_FAULT)r->backend_error=r->factory->config.catalog->error;else if(r->factory->config.family->phase==FAMILY_FAULT)r->backend_error=r->factory->config.family->error;r->failed_at=r->state;r->state=EVENT_ROLL_FAILED;}
uint64_t storage_event_roll_file_id(uint64_t session,uint64_t ordinal){
 const uint64_t h=UINT64_C(1)<<63;
 if(!session||!ordinal||ordinal>=h)return 0;
 uint64_t b=(session&UINT64_C(0xffffffffffff))<<16;return ((b&h)^h)|ordinal;
}
bool storage_event_roll_active(const StorageEventRoll *r){return r&&r->state>EVENT_ROLL_IDLE&&r->state<EVENT_ROLL_DONE;}
StorageResult storage_event_roll_status(const StorageEventRoll *r,uint64_t *file){
 if(!r)return STORE_INVALID;
 if(r->state==EVENT_ROLL_FAILED)return r->error;
 if(r->state!=EVENT_ROLL_DONE)return STORE_BUSY;
 if(file)*file=r->next.file_id;
 return STORE_OK;
}
StorageResult storage_event_roll_request(StorageEventRoll *r,StorageFactory *f,StorageFileHandles *h,StorageNamespace *ns,const StorageEventRollOps *ops,unsigned file,unsigned handle,const char *catalog_path,uint64_t ordinal,uint64_t created){
 if(!r||!f||!h||!ns||!ops||!ops->mkdir||!ops->create||!ops->sync_media||!catalog_path||!f->config.is_owner||!f->config.is_owner(f->config.owner_context)||file>=f->config.count||handle>=STORAGE_HANDLES)return STORE_INVALID;
 if(r->state!=EVENT_ROLL_IDLE&&r->state!=EVENT_ROLL_DONE)return STORE_BUSY;
 StorageFactoryConfig *c=&f->config;StorageFactoryFile *a=&c->files[file];StorageFileCatalog *cat=c->catalog;StorageHandle *old=&h->files[handle];uint64_t id=storage_event_roll_file_id(c->session_id,ordinal);
 if(!id||a->descriptor.role!=CATALOG_EVENT||old->spec.file_id!=a->descriptor.file_id||!old->opened||!old->active||old->spec.kind!=HANDLE_DATA)return STORE_INVALID;
 if(f->state!=FACTORY_READY||c->engine->state!=ENGINE_ACTIVE||cat->phase!=CATALOG_IDLE||cat->epoch_handoff||old->references||old->closing||old->claimed||ns->writing)return STORE_BUSY;
 if(cat->count+cat->history_count>=STORAGE_FAMILY_FILES)return STORE_CAPACITY;
 if((a->service->observer.admission||a->service->observer.terminal)&&!ops->observer)return STORE_INVALID;
 for(unsigned k=0;k<cat->count;k++)if(cat->files[k].descriptor.file_id==id||(cat->files[k].descriptor.location==CATALOG_EVENT_TREE&&cat->files[k].descriptor.ordinal>=ordinal))return STORE_INVALID;
 for(unsigned k=0;k<cat->history_count;k++)if(cat->history[k].descriptor.file_id==id||(cat->history[k].descriptor.location==CATALOG_EVENT_TREE&&cat->history[k].descriptor.ordinal>=ordinal))return STORE_INVALID;
 StorageFileDescriptor next=a->descriptor;next.file_id=id;next.location=CATALOG_EVENT_TREE;next.ordinal=ordinal;next.created_known=true;next.created_us=created;
 StorageHandleSpec spec=old->spec;spec.file_id=id;spec.order_key=created?created:ordinal;
 if(!storage_file_catalog_resolve(catalog_path,&next,spec.path,sizeof(spec.path)))return STORE_INVALID;
 StoragePumpHealth ph;StorageResult result=storage_pump_health(a->pump,&ph);if(result!=STORE_OK)return result;
 if(ph.occupied||ph.admin_pending||ph.fatal||a->service->health.pending||a->service->pcm_pending)return STORE_BUSY;
 result=storage_pump_close(a->pump);if(result!=STORE_OK)return result;
 memset(r,0,sizeof(*r));r->factory=f;r->handles=h;r->space=ns;r->ops=*ops;r->file=file;r->handle=handle;r->old_file_id=a->descriptor.file_id;r->next=next;r->spec=spec;r->state=EVENT_ROLL_LEASE;return STORE_OK;
}
static bool io(StorageEventRoll *r,StorageIoResult result){if(result==STORE_IO_OK)return true;r->backend_error=result==STORE_IO_NO_SPACE?STORE_ERROR_NO_SPACE:result==STORE_IO_NO_MEDIA?STORE_ERROR_NO_MEDIA:STORE_ERROR_IO;fail(r,result==STORE_IO_NO_SPACE?STORE_CAPACITY:STORE_INVALID);return false;}
void storage_event_roll_step(StorageEventRoll *r,uint64_t now){
 if(!storage_event_roll_active(r))return;
 StorageFactoryConfig *c=&r->factory->config;if(!c->is_owner(c->owner_context))return;
 if((r->have_time&&now<r->last_now)||c->family->phase==FAMILY_FAULT||c->catalog->phase==CATALOG_FAULT){fail(r,STORE_INVALID);return;}
 r->have_time=true;r->last_now=now;r->steps++;
 for(unsigned k=0;k<c->count;k++)if(c->files[k].service->health.fatal){fail(r,STORE_INVALID);return;}
 /* Fixed 1:1 fairness, independent of MON queue pressure. A single pump step
  * also advances the shared family transaction. The EVENT slot is excluded
  * while its writer/registry are being replaced. */
 if((r->steps&1)&&c->count>1){unsigned k;do{k=r->round_robin++%c->count;}while(k==r->file);storage_pump_step(c->files[k].pump,now);return;}
 StorageFactoryFile *a=&c->files[r->file];StorageFileCatalog *cat=c->catalog;StorageResult result;
 switch(r->state){
 case EVENT_ROLL_LEASE:
  result=storage_namespace_write_begin(r->space,&r->lease);if(result==STORE_BUSY)return;if(result!=STORE_OK){fail(r,result);return;}r->state=EVENT_ROLL_DRAIN;return;
 case EVENT_ROLL_DRAIN:
  storage_pump_step(a->pump,now);
  if(!a->service->health.drained||!a->service->health.sealed)return;
  {StorageQueueHealth health;unsigned definitions=0;
   if(storage_queue_health(a->queue,&health)!=STORE_OK)return;
   for(unsigned k=0;k<STORAGE_RECEIPTS;k++)if(a->service->slots[k].occupied){StorageServiceSlot *slot=&a->service->slots[k];if(!slot->definition||slot->state!=STORE_SYNCED||!slot->observed)return;definitions++;}
   /* Definition receipts stay pinned through bootstrap planning. Every other
    * public receipt must already have been delivered and explicitly ACKed. */
   if(health.live_jobs||health.retained_receipts!=definitions)return;
  }
  result=storage_file_catalog_request_seal(cat,r->old_file_id,now,true);if(result==STORE_BUSY)return;if(result!=STORE_OK){fail(r,result);return;}r->state=EVENT_ROLL_SEAL;return;
 case EVENT_ROLL_SEAL:
  storage_file_catalog_step(cat,now);if(cat->phase==CATALOG_FAULT){fail(r,STORE_INVALID);return;}if(cat->phase==CATALOG_IDLE)r->state=EVENT_ROLL_COPY;return;
 case EVENT_ROLL_COPY:
  if(c->family->phase!=FAMILY_READY)return;
  result=storage_bootstrap_plan(a->plan,a->service,c->family,r->old_file_id,a->config_id,a->capture_id);if(result!=STORE_OK){fail(r,result);return;}r->state=EVENT_ROLL_RELEASE;return;
 case EVENT_ROLL_RELEASE:
  if(r->release_slot==STORAGE_RECEIPTS){r->state=EVENT_ROLL_ACK;return;}
  if(a->service->slots[r->release_slot].occupied){result=storage_service_release(a->service,a->service->slots[r->release_slot].ticket);if(result==STORE_BUSY)return;if(result!=STORE_OK){fail(r,result);return;}}
  r->release_slot++;return;
 case EVENT_ROLL_ACK:{
  StorageQueueHealth health;result=storage_queue_health(a->queue,&health);if(result==STORE_BUSY)return;if(result!=STORE_OK){fail(r,result);return;}
  if(health.live_jobs||health.retained_receipts)return;
  result=storage_family_queue_release_file(c->owner,r->file);if(result==STORE_BUSY)return;if(result!=STORE_OK){fail(r,result);return;}
  result=storage_file_handles_seal(r->handles,r->handle,&cat->files[r->file]);if(result==STORE_BUSY)return;if(result!=STORE_OK){fail(r,result);return;}r->state=EVENT_ROLL_CLOSE;return;}
 case EVENT_ROLL_CLOSE:
  result=storage_file_handles_close(r->handles,r->handle);if(result==STORE_BUSY)return;if(result!=STORE_OK){fail(r,result);return;}
  if(storage_file_handles_finish_close(r->handles,r->handle)!=STORE_OK){fail(r,STORE_INVALID);return;}r->state=EVENT_ROLL_REPLACE_HANDLE;return;
 case EVENT_ROLL_REPLACE_HANDLE:{
  if(storage_file_handles_forget(r->handles,r->handle)!=STORE_OK){fail(r,STORE_INVALID);return;}
  unsigned slot=STORAGE_HANDLES;result=storage_file_handles_register(r->handles,&r->spec,&slot);if(result!=STORE_OK||slot!=r->handle){fail(r,STORE_INVALID);return;}
  strcpy(r->directory,r->spec.path);char *ev=strstr(r->directory,"/EV/");if(!ev){fail(r,STORE_INVALID);return;}r->dir_cursor=(unsigned)(ev-r->directory)+3;r->state=EVENT_ROLL_MKDIR;return;}
 case EVENT_ROLL_MKDIR:{
  char saved=r->directory[r->dir_cursor];r->directory[r->dir_cursor]=0;StorageIoResult v=r->ops.mkdir(r->ops.context,r->directory);r->directory[r->dir_cursor]=saved;if(!io(r,v))return;
  const char *slash=strchr(r->directory+r->dir_cursor+1,'/');
  if(slash)r->dir_cursor=(unsigned)(slash-r->directory);else r->state=EVENT_ROLL_CREATE;
  return;}
 case EVENT_ROLL_CREATE:
  result=r->ops.create(r->ops.context,r->handles,r->handle);if(result==STORE_BUSY)return;if(result!=STORE_OK){fail(r,result);return;}
  if(!r->handles->files[r->handle].opened||!r->handles->files[r->handle].active||!r->handles->files[r->handle].backend.sync){fail(r,STORE_INVALID);return;}r->state=EVENT_ROLL_SYNC_FILE;return;
 case EVENT_ROLL_SYNC_FILE:{StorageBackend *b=&r->handles->files[r->handle].backend;if(!b->sync||!io(r,b->sync(b->context)))return;r->state=EVENT_ROLL_SYNC_MEDIA;return;}
 case EVENT_ROLL_SYNC_MEDIA:
  if(!io(r,r->ops.sync_media(r->ops.context,r->handles)))return;
  for(unsigned k=0;k<STORAGE_RECEIPTS;k++)r->writer_generation[k]=a->writer->slots[k].generation;
  a->raw=r->handles->files[r->handle].backend;
  if(!storage_binding_init(a->binding,&a->raw)||!storage_writer_init(a->writer,&a->binding->backend,c->session_id,r->next.file_id,c->owner->sequence.next_seq,c->sync_us)){fail(r,STORE_INVALID);return;}
  for(unsigned k=0;k<STORAGE_RECEIPTS;k++)a->writer->slots[k].generation=r->writer_generation[k];
  r->state=EVENT_ROLL_WRITER;return;
 case EVENT_ROLL_WRITER:
  storage_writer_step(a->writer,now);if(a->writer->file_bytes||a->writer->prefix||(a->writer->state!=SW_SCAN_SIZE&&a->writer->state!=SW_SCAN_HEADER&&a->writer->state!=SW_READY)){fail(r,STORE_INVALID);return;}if(a->writer->state==SW_READY)r->state=EVENT_ROLL_BIND;return;
 case EVENT_ROLL_BIND:
  result=storage_queue_rebind_empty(a->queue,a->writer);if(result==STORE_BUSY)return;if(result!=STORE_OK){fail(r,result);return;}
  if(!storage_service_init(a->service,a->queue,&a->binding->registry)){fail(r,STORE_INVALID);return;}
  if(r->ops.observer&&!r->ops.observer(r->ops.context,a->service,r->next.file_id)){fail(r,STORE_INVALID);return;}
  result=storage_pump_rebind_empty(a->pump,a->service);if(result!=STORE_OK){fail(r,result);return;}
  c->family->closed_files&=(uint8_t)~(1u<<c->owner->ports[r->file].file);
  a->descriptor=r->next;result=storage_file_catalog_replace_event(cat,r->file,&r->next);if(result!=STORE_OK){fail(r,result);return;}r->state=EVENT_ROLL_OPEN;return;
 case EVENT_ROLL_OPEN:
  storage_file_catalog_step(cat,now);if(cat->phase==CATALOG_FAULT){fail(r,STORE_INVALID);return;}if(cat->phase!=CATALOG_IDLE)return;
  if(!storage_bootstrap_init(&r->bootstrap,a->plan,a->service)){fail(r,STORE_INVALID);return;}r->state=EVENT_ROLL_BOOTSTRAP;return;
 case EVENT_ROLL_BOOTSTRAP:
  storage_bootstrap_step(&r->bootstrap);if(r->bootstrap.state==BOOTSTRAP_FAILED){fail(r,STORE_DEPENDENCY);return;}
  storage_pump_step(a->pump,now);
  if(r->bootstrap.state!=BOOTSTRAP_DONE||!storage_service_capture_ready(a->service,c->session_id,a->config_id,a->capture_id))return;
  result=storage_namespace_write_end(r->space,r->lease);if(result!=STORE_OK){fail(r,result);return;}r->lease=0;r->state=EVENT_ROLL_DONE;return;
 default:return;
 }
}
