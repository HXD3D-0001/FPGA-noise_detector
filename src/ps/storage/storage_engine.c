#include "storage_engine.h"
#include <string.h>
static bool init(StorageEngine *e,StorageFileCatalog *c,StoragePump **p,unsigned n,StorageQuery *q,StorageDelete *d,StorageEpoch *epoch,StorageBootstrapPlan **plans,const uint32_t *config,const uint64_t *capture,const StorageEpochHooks *h,uint64_t created,uint64_t bytes,uint64_t age){
 if(!e||!c||c->count!=n||!n||n>STORAGE_FAMILY_FILES||!p||!epoch||!plans||!!config!=!!capture||!h||!h->quiescent||!h->close||!bytes||!age)return false;
 for(unsigned k=0;k<n;k++)if(!p[k]||p[k]->service!=c->services[k]||!plans[k]||(config&&(!config[k]||!capture[k])))return false;
 StorageEpochHooks hooks=*h;memset(e,0,sizeof(*e));e->catalog=c;e->count=n;e->query=q;e->deletion=d;e->epoch=epoch;e->hooks=hooks;e->max_file_bytes=bytes;e->max_file_age_us=age;e->created_us=created;
 for(unsigned k=0;k<n;k++){e->pumps[k]=p[k];e->plans[k]=plans[k];e->config[k]=config?config[k]:0;e->capture[k]=capture?capture[k]:0;}e->state=config?ENGINE_ACTIVE:ENGINE_AWAIT_CAPTURE;return true;
}
bool storage_engine_init(StorageEngine *e,StorageFileCatalog *c,StoragePump **p,unsigned n,StorageQuery *q,StorageDelete *d,StorageEpoch *epoch,StorageBootstrapPlan **plans,const uint32_t *config,const uint64_t *capture,const StorageEpochHooks *h,uint64_t created,uint64_t bytes,uint64_t age){
 return config&&capture&&init(e,c,p,n,q,d,epoch,plans,config,capture,h,created,bytes,age);
}
bool storage_engine_init_unbound(StorageEngine *e,StorageFileCatalog *c,StoragePump **p,unsigned n,StorageEpoch *epoch,StorageBootstrapPlan **plans,const StorageEpochHooks *h,uint64_t created,uint64_t bytes,uint64_t age){
 return init(e,c,p,n,NULL,NULL,epoch,plans,NULL,NULL,h,created,bytes,age);
}
StorageResult storage_engine_bind_capture(StorageEngine *e,uint32_t config,uint64_t capture){
 if(!e||!config||!capture)return STORE_INVALID;
 if(e->state!=ENGINE_AWAIT_CAPTURE||e->catalog->phase!=CATALOG_IDLE)return STORE_BUSY;
 for(unsigned k=0;k<e->count;k++)if(!storage_service_capture_ready(e->pumps[k]->service,e->catalog->owner->family->session_id,config,capture))return STORE_DEPENDENCY;
 for(unsigned k=0;k<e->count;k++){e->config[k]=config;e->capture[k]=capture;}
 e->state=ENGINE_ACTIVE;return STORE_OK;
}
StorageResult storage_engine_request_rotation(StorageEngine *e){
 if(!e||e->state!=ENGINE_ACTIVE||e->catalog->phase!=CATALOG_IDLE)return STORE_BUSY;
 if(!e->hooks.quiescent(e->hooks.context))return STORE_BUSY;
 /* Producers are externally quiescent, but lock contention may leave a prefix
  * of pumps closed. Retry continues closing all; cannot reopen admissions. */
 e->state=ENGINE_DRAINING;return STORE_OK;
}
void storage_engine_step(StorageEngine *e,uint64_t now){
 if(!e||e->state==ENGINE_HANDOFF||e->state==ENGINE_FAILED)return;
 e->steps++;StorageFileCatalog *c=e->catalog;
 if(now<e->created_us||c->phase==CATALOG_FAULT||c->owner->family->phase==FAMILY_FAULT){e->state=ENGINE_FAILED;return;}
 for(unsigned k=0;k<e->count;k++)if(e->pumps[k]->service->health.fatal){e->state=ENGINE_FAILED;return;}
 if(e->state==ENGINE_AWAIT_CAPTURE){storage_pump_step(e->pumps[e->round_robin++%e->count],now);return;}
 if(e->state==ENGINE_ACTIVE){
  if(c->phase!=CATALOG_IDLE){storage_file_catalog_step(c,now);return;}
  bool urgent=false;
  for(unsigned k=0;k<e->count;k++){StorageService *s=e->pumps[k]->service;StorageQueueHealth h;
   if(s->queue->writer->file_bytes>=e->max_file_bytes||now-e->created_us>=e->max_file_age_us)e->rotation_due=true;
   if(storage_queue_health(s->queue,&h)!=STORE_OK||h.live_jobs>=8||s->unsaved.count||s->audit_active||s->pcm_pending)urgent=true;
  }
  /* Pending deletion transaction owns its victim exclusion until completion.
   * It advances only on background slices, so capture writes remain serviceable. */
  if(!urgent&&++e->budget>=16){e->budget=0;if(e->deletion&&e->deletion->state!=DELETE_IDLE&&e->deletion->state!=DELETE_FAULT){storage_delete_step(e->deletion);return;}if(e->query){storage_query_step(e->query);return;}}
  storage_pump_step(e->pumps[e->round_robin++%e->count],now);return;
 }
 if(e->state==ENGINE_DRAINING){
  if(!e->hooks.quiescent(e->hooks.context)){e->state=ENGINE_FAILED;return;}
  for(unsigned k=0;k<e->count;k++){StorageResult r=storage_pump_close(e->pumps[k]);if(r==STORE_BUSY)return;if(r!=STORE_OK){e->state=ENGINE_FAILED;return;}}
  bool drained=true;for(unsigned k=0;k<e->count;k++){StoragePumpHealth h;if(storage_pump_health(e->pumps[k],&h)!=STORE_OK||!h.drained||h.occupied)drained=false;}
  /* Finish background delete before epoch closes any backend. */
  if(e->deletion&&e->deletion->state!=DELETE_IDLE){if(e->deletion->state==DELETE_FAULT){e->state=ENGINE_FAILED;return;}if(++e->budget>=16){e->budget=0;storage_delete_step(e->deletion);return;}drained=false;}
  if(!drained){storage_pump_step(e->pumps[e->round_robin++%e->count],now);return;}
  if(!storage_epoch_init(e->epoch,c,e->plans,e->config,e->capture,&e->hooks)){e->state=ENGINE_FAILED;return;}e->state=ENGINE_ROTATING;return;
 }
 if(e->state==ENGINE_ROTATING){
  if(e->query&&++e->budget>=16){e->budget=0;storage_query_step(e->query);return;}
  storage_epoch_step(e->epoch,now);if(e->epoch->state==EPOCH_FAILED)e->state=ENGINE_FAILED;else if(e->epoch->state==EPOCH_READY)e->state=ENGINE_HANDOFF;
 }
}
