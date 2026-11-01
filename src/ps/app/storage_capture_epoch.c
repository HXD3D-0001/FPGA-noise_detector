#include "storage_capture_epoch.h"
#include <string.h>
static bool owner(StorageCaptureEpoch *p){return p&&p->handles&&p->handles->ops.is_owner&&p->handles->ops.is_owner(p->handles->ops.context)&&p->space&&storage_namespace_version(p->space);}
static bool quiescent(void *ctx){
 StorageCaptureEpoch *p=ctx;if(!owner(p)||!p->producer_stopped(p->proof_context))return false;
 if(p->state==CAPTURE_EPOCH_PREPARE)return true;
 if(p->state!=CAPTURE_EPOCH_STOPPING&&p->state!=CAPTURE_EPOCH_HANDOFF&&p->state!=CAPTURE_EPOCH_DONE)return false;
 CaptureRouteHealth h;return capture_storage_route_health(p->route,&h)&&h.attached&&h.producer_closed&&h.consumer.handoff_drained&&!h.consumer.blocked;
}
static bool same_backend(const StorageBackend *a,const StorageBackend *b){return a->context==b->context&&a->read==b->read&&a->write==b->write&&a->sync==b->sync;}
static StorageResult close_epoch(void *ctx,uint64_t id){
 StorageCaptureEpoch *p=ctx;if(!owner(p)||!quiescent(p)||!p->space->writing||p->lifecycle->lease!=p->space->version)return STORE_BUSY;
 StorageFactoryConfig *c=&p->factory->config;if(c->count!=p->count)return STORE_INVALID;
 unsigned at=STORAGE_PROVISION_FILES;
 for(unsigned k=0;k<p->count+2;k++){
  StorageHandle *e=&p->handles->files[p->slots[k]];uint64_t expected=k<p->count?c->files[k].descriptor.file_id:k==p->count?c->ledger_id:c->catalog_id;
  const StorageBackend *b=k<p->count?&c->files[k].raw:k==p->count?&c->ledger:&c->manifest;
  if(e->spec.file_id!=expected||e->spec.session_id!=c->session_id||(e->opened&&!same_backend(&e->backend,b)))return STORE_INVALID;
  if(expected==id)at=k;
 }
 if(at==STORAGE_PROVISION_FILES)return STORE_INVALID;
 StorageResult r=storage_file_handles_freeze_epoch(p->handles,c->catalog,p->slots);if(r!=STORE_OK)return r;p->frozen=true;
 return storage_file_handles_close(p->handles,p->slots[at]);
}
bool storage_capture_epoch_init(StorageCaptureEpoch *p,StorageFileHandles *h,StorageFactory *f,StorageLifecycle *l,StorageNamespace *s,const unsigned *slots,unsigned n,bool (*stopped)(void *),void *ctx,StorageEpochHooks *out){
 if(!p||!h||!f||!l||!s||!slots||!n||n>STORAGE_FAMILY_FILES||!stopped||!out||!h->ops.is_owner||!h->ops.is_owner(h->ops.context)||!storage_namespace_version(s))return false;
 unsigned order[STORAGE_PROVISION_FILES]={0};for(unsigned k=0;k<n+2;k++){if(slots[k]>=STORAGE_HANDLES||!h->files[slots[k]].used)return false;for(unsigned j=0;j<k;j++)if(slots[j]==slots[k])return false;order[k]=slots[k];}
 memset(p,0,sizeof(*p));p->handles=h;p->factory=f;p->lifecycle=l;p->space=s;p->count=n;p->proof_context=ctx;p->producer_stopped=stopped;memcpy(p->slots,order,sizeof(order));*out=(StorageEpochHooks){p,quiescent,close_epoch};return true;
}
static bool exact_pumps(const StorageFactoryConfig *c,const StorageMultiSink *s){
 if(s->close_count!=c->count)return false;
 for(unsigned k=0;k<c->count;k++){
  unsigned matches=0;for(unsigned j=0;j<s->close_count;j++)if(s->close_pumps[j]==c->files[k].pump)matches++;
  if(matches!=1)return false;
 }
 return true;
}
static StorageResult ready(StorageCaptureEpoch *p,CaptureStorageRoute *r,StorageMultiSink *sink,bool reconnect){
 if(!owner(p)||p->state!=CAPTURE_EPOCH_PREPARE||!r||!sink||!p->producer_stopped(p->proof_context))return STORE_BUSY;
 StorageFactoryConfig *c=&p->factory->config;CaptureRouteHealth h;
 if(p->factory->state!=FACTORY_READY||p->lifecycle->state!=LIFECYCLE_RUNNING||p->lifecycle->factory!=p->factory||p->lifecycle->space!=p->space||c->count!=p->count||c->hooks.context!=p||c->hooks.quiescent!=quiescent||c->hooks.close!=close_epoch||sink->session_id!=c->session_id||r->session_id!=c->session_id||sink->active||sink->closing||(!reconnect&&!storage_multi_sink_matches(sink,&r->sink))||!capture_storage_route_health(r,&h)||!h.attached||h.producer_closed!=reconnect||h.consumer.blocked||h.consumer.handoff_drained!=reconnect)return STORE_INVALID;
 StoragePump *targets[]={sink->targets.energy,sink->targets.psd,sink->targets.pcm,sink->targets.raw};
 if(!exact_pumps(c,sink))return STORE_INVALID;
 for(unsigned k=0;k<4;k++){bool found=false;for(unsigned j=0;j<c->count;j++)if(targets[k]==c->files[j].pump)found=true;if(!found)return STORE_INVALID;}
 return STORE_OK;
}
StorageResult storage_capture_epoch_attach(StorageCaptureEpoch *p,CaptureStorageRoute *r,StorageMultiSink *sink){
 StorageResult result=ready(p,r,sink,false);if(result!=STORE_OK)return result;
 p->route=r;p->sink=sink;p->state=CAPTURE_EPOCH_ACTIVE;return STORE_OK;
}
StorageResult storage_capture_epoch_stop(StorageCaptureEpoch *p){
 if(!owner(p)||p->state!=CAPTURE_EPOCH_ACTIVE)return STORE_BUSY;
 StorageResult r=storage_lifecycle_request_stop(p->lifecycle);if(r==STORE_OK)p->state=CAPTURE_EPOCH_STOPPING;return r;
}
void storage_capture_epoch_step(StorageCaptureEpoch *p,uint64_t now){
 if(!owner(p)||(p->state!=CAPTURE_EPOCH_ACTIVE&&p->state!=CAPTURE_EPOCH_STOPPING))return;
 if(p->segment){
  storage_segment_step(p->segment);
  if(p->segment->state==SEGMENT_FAILED){p->error=p->segment->error;p->state=CAPTURE_EPOCH_FAILED;p->lifecycle->state=LIFECYCLE_FAILED;return;}
  if(p->segment->state==SEGMENT_SYNCED){
   RecordIdentity *id=&p->segment->definitions[1].identity;
   for(unsigned k=0;k<p->count;k++){p->factory->config.files[k].config_id=id->config_id;p->factory->config.files[k].capture_id=id->capture_id;p->factory->config.engine->config[k]=id->config_id;p->factory->config.engine->capture[k]=id->capture_id;}
  }
 }
 if(!p->segment||p->segment->state==SEGMENT_IDLE||p->segment->state==SEGMENT_SYNCED)capture_storage_route_step(p->route);
 CaptureRouteHealth h;if(capture_storage_route_health(p->route,&h)&&h.consumer.blocked){p->error=h.consumer.error;p->state=CAPTURE_EPOCH_FAILED;p->lifecycle->state=LIFECYCLE_FAILED;return;}
 storage_lifecycle_step(p->lifecycle,now);
 if(p->lifecycle->state==LIFECYCLE_FAILED){p->error=STORE_INVALID;p->state=CAPTURE_EPOCH_FAILED;}else if(p->lifecycle->state==LIFECYCLE_HANDOFF)p->state=CAPTURE_EPOCH_HANDOFF;
}
StorageResult storage_capture_epoch_finish(StorageCaptureEpoch *p){
 if(!owner(p)||p->state!=CAPTURE_EPOCH_HANDOFF||p->lifecycle->state!=LIFECYCLE_HANDOFF||!p->frozen)return STORE_BUSY;
 StorageResult r=storage_file_handles_finish_epoch(p->handles,p->factory->config.catalog,p->slots);if(r==STORE_OK)p->state=CAPTURE_EPOCH_DONE;return r;
}

StorageResult storage_capture_epoch_reconnect(StorageCaptureEpoch *old,StorageCaptureEpoch *next,CaptureStorageRoute*r,StorageMultiSink*s){
 if(!owner(old)||!owner(next)||old==next||old->state!=CAPTURE_EPOCH_DONE||old->route!=r||!old->sink||old->sink==s||old->space!=next->space||!old->sink->closing||old->sink->active||!s||s->session_id!=old->sink->session_id||s->next_ticket<old->sink->next_ticket)return STORE_INVALID;
 StorageResult result=ready(next,r,s,true);if(result!=STORE_OK)return result;
 CaptureRouteHealth h;if(!capture_storage_route_health(r,&h))return STORE_BUSY;
 CaptureStorageSink v={0};/* init already supplied this exact new adapter. */
 if(!storage_multi_sink_view(s,&v))return STORE_INVALID;
 result=capture_storage_route_reconnect(r,&v,h.generation);if(result!=STORE_OK)return result;
 next->route=r;next->sink=s;next->state=CAPTURE_EPOCH_ACTIVE;return STORE_OK;
}

StorageResult storage_capture_epoch_segment(StorageCaptureEpoch *p,StorageSegment *s){
 if(!owner(p)||!s||s->state!=SEGMENT_IDLE||p->segment||(p->state!=CAPTURE_EPOCH_ACTIVE&&p->state!=CAPTURE_EPOCH_PREPARE)||s->count!=p->count||s->session_id!=p->factory->config.session_id)return STORE_INVALID;
 for(unsigned k=0;k<p->count;k++)if(s->services[k]!=p->factory->config.files[k].service)return STORE_INVALID;
 p->segment=s;return STORE_OK;
}

StorageResult storage_capture_epoch_stage_first(StorageCaptureEpoch *p,CaptureStorageRoute *r,StorageMultiSink *sink){
 if(!owner(p)||p->state!=CAPTURE_EPOCH_PREPARE||!p->producer_stopped(p->proof_context))return STORE_BUSY;
 StorageFactoryConfig *c=&p->factory->config;CaptureRouteHealth h;
 if(!r||!sink||!p->segment||p->segment->state!=SEGMENT_IDLE||p->factory->state!=FACTORY_READY||!c->await_first_capture||c->engine->state!=ENGINE_AWAIT_CAPTURE||p->lifecycle->state!=LIFECYCLE_METADATA||p->lifecycle->factory!=p->factory||p->lifecycle->space!=p->space||c->count!=p->count||c->hooks.context!=p||c->hooks.quiescent!=quiescent||c->hooks.close!=close_epoch||sink->session_id!=c->session_id||r->session_id!=c->session_id||sink->active||sink->closing||!storage_multi_sink_matches(sink,&r->sink))return STORE_INVALID;
 if(!capture_storage_route_health(r,&h))return STORE_BUSY;
 if(!h.attached||h.generation!=1||h.producer_closed||h.consumer.blocked||h.consumer.handoff_drained)return STORE_INVALID;
 if(!exact_pumps(c,sink))return STORE_INVALID;
 StoragePump *targets[]={sink->targets.energy,sink->targets.psd,sink->targets.pcm,sink->targets.raw};
 for(unsigned k=0;k<4;k++){bool found=false;for(unsigned j=0;j<c->count;j++)if(targets[k]==c->files[j].pump)found=true;if(!found)return STORE_INVALID;}
 p->route=r;p->sink=sink;p->state=CAPTURE_EPOCH_AWAIT_FIRST;return STORE_OK;
}
StorageResult storage_capture_epoch_activate_first(StorageCaptureEpoch *p){
 if(!owner(p)||p->state!=CAPTURE_EPOCH_AWAIT_FIRST)return STORE_BUSY;
 StorageFactoryConfig *c=&p->factory->config;CaptureRouteHealth h;CaptureRouteFacts facts;
 if(!p->segment||p->segment->state!=SEGMENT_SYNCED||p->factory->state!=FACTORY_READY||c->await_first_capture||c->engine->state!=ENGINE_ACTIVE||p->lifecycle->state!=LIFECYCLE_RUNNING||!storage_multi_sink_matches(p->sink,&p->route->sink))return STORE_INVALID;
 if(!capture_storage_route_health(p->route,&h)||!capture_storage_route_facts_copy(p->route,&facts))return STORE_BUSY;
 RecordIdentity *id=&p->segment->definitions[1].identity;
 if(!h.attached||h.generation!=1||h.consumer.blocked||!facts.capture_valid||!facts.started_valid||!facts.active_valid||facts.identity.session_id!=id->session_id||facts.identity.capture_id!=id->capture_id||facts.identity.config.config_id!=id->config_id)return STORE_EXPIRED;
 for(unsigned k=0;k<p->count;k++)if(c->files[k].config_id!=id->config_id||c->files[k].capture_id!=id->capture_id||!storage_service_capture_ready(c->files[k].service,c->session_id,id->config_id,id->capture_id))return STORE_DEPENDENCY;
 p->state=CAPTURE_EPOCH_ACTIVE;return STORE_OK;
}
