#include "storage_factory.h"
#include <string.h>
typedef struct {uintptr_t first,last;} Region;
static bool add(Region *r,unsigned *n,const void *p,size_t bytes){
 if(!p||!bytes||(uintptr_t)p>UINTPTR_MAX-bytes)return false;
 uintptr_t a=(uintptr_t)p,b=a+bytes;
 for(unsigned k=0;k<*n;k++)if(a<r[k].last&&r[k].first<b)return false;
 r[(*n)++]=(Region){a,b};return true;
}
static bool backend(const StorageBackend *b){return b->size&&b->read&&b->write&&b->sync;}
static bool alias(const StorageBackend *a,const StorageBackend *b){return a->context==b->context&&(a->write==b->write||a->read==b->read);}
bool storage_factory_validate(StorageFactory *f,const StorageFactoryConfig *c){
 if(!f||!c||!c->count||c->count>STORAGE_FAMILY_FILES||!c->session_id||!c->next_sequence||c->next_sequence==UINT64_MAX||!c->ledger_id||!c->catalog_id||c->ledger_id==c->catalog_id||!c->sync_us||!c->max_file_bytes||!c->max_file_age_us||!backend(&c->ledger)||!backend(&c->manifest)||alias(&c->ledger,&c->manifest)||!c->hooks.quiescent||!c->hooks.close||!c->is_owner||!c->is_owner(c->owner_context)||!c->hooks.quiescent(c->hooks.context)||!!c->locks.lock!=!!c->locks.unlock||((uintptr_t)c->arena&63))return false;
 Region regions[80];unsigned n=0;
#define REGION(p) if(!add(regions,&n,(p),sizeof(*(p))))return false
 REGION(f);REGION(c->pool);REGION(c->family);REGION(c->owner);REGION(c->catalog);REGION(c->verifier);REGION(c->epoch);REGION(c->engine);
 if(!add(regions,&n,c->arena,STORAGE_POOL_BYTES))return false;
 for(unsigned k=0;k<c->count;k++){
  const StorageFactoryFile *a=&c->files[k];
  if(!backend(&a->raw)||(c->await_first_capture?(a->config_id||a->capture_id):(!a->config_id||!a->capture_id))||!a->descriptor.file_id||a->descriptor.file_id==c->ledger_id||a->descriptor.file_id==c->catalog_id||!memchr(a->descriptor.name,0,64)||!a->descriptor.name[0]||alias(&a->raw,&c->ledger)||alias(&a->raw,&c->manifest))return false;
  /* Catalog checks full descriptor syntax before any OPEN write. */
  for(unsigned j=0;j<k;j++)if(a->descriptor.file_id==c->files[j].descriptor.file_id||!strcmp(a->descriptor.name,c->files[j].descriptor.name)||alias(&a->raw,&c->files[j].raw))return false;
  REGION(a->binding);REGION(a->writer);REGION(a->queue);REGION(a->service);REGION(a->pump);REGION(a->plan);
 }
#undef REGION
 return true;
}
bool storage_factory_init(StorageFactory *f,const StorageFactoryConfig *c){
 if(!storage_factory_validate(f,c))return false;
 StorageFactoryConfig copy=*c;memset(f,0,sizeof(*f));f->config=copy;f->state=FACTORY_CHECK_EMPTY;return true;
}
static void fail(StorageFactory *f,StorageResult r){f->error=r;f->state=FACTORY_FAILED;}
static bool prepare(StorageFactory *f){
 StorageFactoryConfig *c=&f->config;
 if(!storage_queue_pool_init(c->pool,c->arena,STORAGE_POOL_BYTES,&c->locks))return false;
 for(unsigned k=0;k<c->count;k++){StorageFactoryFile *a=&c->files[k];if(!storage_binding_init(a->binding,&a->raw)||!storage_writer_init(a->writer,&a->binding->backend,c->session_id,a->descriptor.file_id,c->next_sequence,c->sync_us))return false;}
 return storage_family_init(c->family,&c->ledger,c->session_id,c->ledger_id,c->next_sequence);
}
static bool bind_all(StorageFactory *f){
 StorageFactoryConfig *c=&f->config;
 for(unsigned k=0;k<c->count;k++)if(!storage_family_attach(c->family,c->files[k].writer))return false;
 if(!storage_family_queue_init(c->owner,c->family,&c->locks))return false;
 for(unsigned k=0;k<c->count;k++){StorageFactoryFile *a=&c->files[k];if(!storage_queue_init_shared(a->queue,a->writer,c->pool)||!storage_family_queue_attach(c->owner,a->queue)||!storage_service_init(a->service,a->queue,&a->binding->registry)||!storage_pump_init(a->pump,a->service,&c->locks))return false;}
 return storage_family_queue_enable_streaming(c->owner);
}
void storage_factory_step(StorageFactory *f,uint64_t now){
 if(!f||f->state==FACTORY_FAILED)return;
 StorageFactoryConfig *c=&f->config;if(!c->is_owner||!c->is_owner(c->owner_context))return;
 if(f->have_time&&now<f->last_now){fail(f,STORE_INVALID);return;}f->have_time=true;f->last_now=now;
 if(f->state!=FACTORY_READY&&!c->hooks.quiescent(c->hooks.context)){fail(f,STORE_BUSY);return;}
 if(f->state==FACTORY_CHECK_EMPTY){
  const StorageBackend *b=f->cursor<c->count?&c->files[f->cursor].raw:f->cursor==c->count?&c->ledger:&c->manifest;
  uint64_t bytes=0;if(b->size(b->context,&bytes)!=STORE_IO_OK||bytes){fail(f,STORE_INVALID);return;}
  if(++f->cursor==c->count+2){if(!prepare(f)){fail(f,STORE_INVALID);return;}f->cursor=0;f->state=FACTORY_WRITERS;}return;
 }
 if(f->state==FACTORY_WRITERS){StorageWriter *w=c->files[f->cursor].writer;if(w->state==SW_READY){if(++f->cursor==c->count)f->state=FACTORY_LEDGER;return;}storage_writer_step(w,now);if(w->file_bytes||w->prefix||(w->state!=SW_SCAN_SIZE&&w->state!=SW_SCAN_HEADER&&w->state!=SW_READY))fail(f,STORE_INVALID);return;}
 if(f->state==FACTORY_LEDGER){storage_family_step(c->family,now);if(c->family->phase!=FAMILY_READY){fail(f,STORE_INVALID);return;}f->state=FACTORY_BIND;return;}
 if(f->state==FACTORY_BIND){
  if(!bind_all(f)){fail(f,STORE_INVALID);return;}
  StorageService *services[8];StorageFileDescriptor descriptors[8];for(unsigned k=0;k<c->count;k++){services[k]=c->files[k].service;descriptors[k]=c->files[k].descriptor;}
  StorageResult r=storage_file_catalog_init(c->catalog,c->owner,services,descriptors,c->count,&c->manifest,c->catalog_id,c->verifier);if(r!=STORE_OK){fail(f,r);return;}f->state=FACTORY_CATALOG;return;
 }
 if(f->state==FACTORY_CATALOG){
  storage_file_catalog_step(c->catalog,now);if(c->catalog->phase==CATALOG_FAULT){fail(f,STORE_INVALID);return;}if(c->catalog->phase!=CATALOG_IDLE)return;
  StoragePump *pumps[8];StorageBootstrapPlan *plans[8];uint32_t config[8];uint64_t capture[8];for(unsigned k=0;k<c->count;k++){pumps[k]=c->files[k].pump;plans[k]=c->files[k].plan;config[k]=c->files[k].config_id;capture[k]=c->files[k].capture_id;}
  if(!(c->await_first_capture?storage_engine_init_unbound(c->engine,c->catalog,pumps,c->count,c->epoch,plans,&c->hooks,c->created_us,c->max_file_bytes,c->max_file_age_us):storage_engine_init(c->engine,c->catalog,pumps,c->count,NULL,NULL,c->epoch,plans,config,capture,&c->hooks,c->created_us,c->max_file_bytes,c->max_file_age_us))){fail(f,STORE_INVALID);return;}f->state=FACTORY_READY;return;
 }
 if(f->state==FACTORY_BOOTSTRAP){
  bool done=true;for(unsigned k=0;k<c->count;k++){storage_bootstrap_step(&f->bootstrap[k]);if(f->bootstrap[k].state==BOOTSTRAP_FAILED){fail(f,STORE_DEPENDENCY);return;}if(f->bootstrap[k].state!=BOOTSTRAP_DONE)done=false;}
  storage_engine_step(c->engine,now);if(c->engine->state==ENGINE_FAILED){fail(f,STORE_INVALID);return;}if(done)f->state=FACTORY_READY;return;
 }
 if(f->state==FACTORY_READY){storage_engine_step(c->engine,now);if(c->engine->state==ENGINE_FAILED)fail(f,STORE_INVALID);}
}
bool storage_factory_bootstrap(StorageFactory *f,const StorageBootstrapPlan *const *plans){
 if(!f||f->state!=FACTORY_READY||f->config.await_first_capture||!plans)return false;
 StorageFactoryConfig *c=&f->config;if(!c->is_owner(c->owner_context)||!c->hooks.quiescent(c->hooks.context))return false;
 for(unsigned k=0;k<c->count;k++){StorageService *s=c->files[k].service;StorageQueueHealth h;if(!plans[k]||!plans[k]->valid||!plans[k]->count||plans[k]->count>STORAGE_BOOTSTRAP_DEFINITIONS||plans[k]->session_id!=c->session_id||atomic_load(&s->registry->published)||storage_queue_health(s->queue,&h)!=STORE_OK||h.accepted||h.retained_receipts||h.live_jobs)return false;}
 for(unsigned k=0;k<c->count;k++)if(!storage_bootstrap_init(&f->bootstrap[k],plans[k],c->files[k].service)){fail(f,STORE_INVALID);return false;}
 f->state=FACTORY_BOOTSTRAP;return true;
}

StorageResult storage_factory_bind_first_capture(StorageFactory *f,uint32_t config,uint64_t capture){
 if(!f||!config||!capture||!f->config.is_owner||!f->config.is_owner(f->config.owner_context))return STORE_INVALID;
 if(f->state!=FACTORY_READY||!f->config.await_first_capture)return STORE_BUSY;
 StorageResult result=storage_engine_bind_capture(f->config.engine,config,capture);if(result!=STORE_OK)return result;
 for(unsigned k=0;k<f->config.count;k++){f->config.files[k].config_id=config;f->config.files[k].capture_id=capture;}
 f->config.await_first_capture=false;return STORE_OK;
}
