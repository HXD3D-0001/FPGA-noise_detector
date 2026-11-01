#include "storage_lifecycle.h"
#include <string.h>
static bool owned(StorageNamespace *n){return n&&n->is_owner&&n->is_owner(n->context);}
static void fail(StorageLifecycle *l){l->state=LIFECYCLE_FAILED;}
static bool take(StorageLifecycle *l){if(l->lease)return true;StorageResult r=storage_namespace_write_begin(l->space,&l->lease);if(r==STORE_BUSY)return false;if(r!=STORE_OK)fail(l);return r==STORE_OK;}
static bool drop(StorageLifecycle *l){if(!l->lease)return true;if(storage_namespace_write_end(l->space,l->lease)!=STORE_OK){fail(l);return false;}l->lease=0;return true;}
bool storage_lifecycle_init(StorageLifecycle *l,StorageFactory *f,StorageNamespace *n){
 if(!l||!f||!owned(n)||f->state!=FACTORY_CHECK_EMPTY||!f->config.is_owner||!f->config.is_owner(f->config.owner_context))return false;
 memset(l,0,sizeof(*l));l->factory=f;l->space=n;l->state=LIFECYCLE_ASSEMBLE;return true;
}
StorageResult storage_lifecycle_adopt_write(StorageLifecycle *l,uint64_t token){
 if(!l||!owned(l->space)||l->state!=LIFECYCLE_ASSEMBLE||l->factory->state!=FACTORY_CHECK_EMPTY||l->have_time||l->lease||!token||!l->space->writing||l->space->reading||token!=l->space->version)return STORE_INVALID;
 l->lease=token;return STORE_OK;
}
StorageResult storage_lifecycle_bootstrap(StorageLifecycle *l,const StorageBootstrapPlan *const *p){
 if(!l||l->state!=LIFECYCLE_METADATA||!owned(l->space)||l->factory->state!=FACTORY_READY)return STORE_BUSY;
 if(!take(l))return l->state==LIFECYCLE_FAILED?STORE_EXHAUSTED:STORE_BUSY;
 if(!storage_factory_bootstrap(l->factory,p)){(void)drop(l);return STORE_INVALID;}return STORE_OK;
}
StorageResult storage_lifecycle_enable(StorageLifecycle *l){
 if(!l||!owned(l->space)||l->state!=LIFECYCLE_METADATA||l->factory->state!=FACTORY_READY||(l->space->writing&&!l->lease))return STORE_BUSY;
 StorageFactoryConfig *c=&l->factory->config;
 if(c->await_first_capture||c->engine->state!=ENGINE_ACTIVE)return STORE_BUSY;
 for(unsigned k=0;k<c->count;k++)if(!storage_service_capture_ready(c->files[k].service,c->session_id,c->files[k].config_id,c->files[k].capture_id))return STORE_BUSY;
 if(!drop(l))return STORE_INVALID;
 l->state=LIFECYCLE_RUNNING;return STORE_OK;
}
StorageResult storage_lifecycle_request_stop(StorageLifecycle *l){if(!l||!owned(l->space)||l->state!=LIFECYCLE_RUNNING)return STORE_BUSY;l->state=LIFECYCLE_WAIT_STOP;return STORE_OK;}
static void pump_only(StorageLifecycle *l,uint64_t now){StorageFactoryConfig *c=&l->factory->config;storage_pump_step(c->files[l->round_robin++%c->count].pump,now);for(unsigned k=0;k<c->count;k++)if(c->files[k].service->health.fatal)fail(l);}
void storage_lifecycle_step(StorageLifecycle *l,uint64_t now){
 if(!l||l->state==LIFECYCLE_FAILED||l->state==LIFECYCLE_HANDOFF||!owned(l->space))return;
 StorageFactory *f=l->factory;StorageFactoryConfig *c=&f->config;
 if(l->have_time&&now<l->last_now){fail(l);return;}l->have_time=true;l->last_now=now;
 if(l->state==LIFECYCLE_ASSEMBLE){
  if(!take(l))return;
  storage_factory_step(f,now);if(f->state==FACTORY_FAILED){fail(l);return;}if(f->state==FACTORY_READY&&drop(l))l->state=LIFECYCLE_METADATA;return;
 }
 if(l->state==LIFECYCLE_WAIT_STOP){
  if(!c->hooks.quiescent(c->hooks.context)||l->space->reading){pump_only(l,now);return;}
  if(!take(l))return;
  if(storage_engine_request_rotation(c->engine)!=STORE_OK){fail(l);return;}l->state=LIFECYCLE_ROTATE;return;
 }
 if(l->state==LIFECYCLE_ROTATE){
  if(!take(l))return;
  storage_factory_step(f,now);if(f->state==FACTORY_FAILED){fail(l);return;}if(c->engine->state==ENGINE_HANDOFF&&drop(l))l->state=LIFECYCLE_HANDOFF;return;
 }
 bool mutation=f->state==FACTORY_BOOTSTRAP||c->catalog->phase!=CATALOG_IDLE||c->engine->state!=ENGINE_ACTIVE||(c->engine->deletion&&c->engine->deletion->state!=DELETE_IDLE);
 if(mutation&&!take(l)){if(l->state!=LIFECYCLE_FAILED)pump_only(l,now);return;}
 if(l->space->reading||(l->space->writing&&!l->lease)){pump_only(l,now);return;}
 storage_factory_step(f,now);if(f->state==FACTORY_FAILED){fail(l);return;}
 if(c->engine->deletion&&c->engine->deletion->state==DELETE_FAULT){fail(l);return;}
 bool pending=f->state==FACTORY_BOOTSTRAP||c->catalog->phase!=CATALOG_IDLE||c->engine->state!=ENGINE_ACTIVE||(c->engine->deletion&&c->engine->deletion->state!=DELETE_IDLE);
 if(!pending)(void)drop(l);
}
