#include "storage_owner.h"
#include "storage_runtime.h"
/* Read from the linked ARM ELF; Host sizeof is not target memory evidence. */
const uint32_t lfn_storage_owner_sizes[]={STORAGE_OWNER_FILES,sizeof(StorageOwner),
 sizeof(StorageRuntimeDiagnostics),STORAGE_POOL_BYTES,STORAGE_OWNER_GROUPS};
#include <string.h>
_Static_assert((unsigned)STORAGE_OWNER_HANDLES<=(unsigned)STORAGE_HANDLES,"owner handle cache exceeds StorageFileHandles");
_Static_assert(STORAGE_OWNER_GROUPS==2,"owner alternates exactly two persistent groups");
_Static_assert((unsigned)STORAGE_OWNER_GROUP_FILES<=(unsigned)STORAGE_FAMILY_FILES,"owner group exceeds family file capacity");
static bool owned(const StorageOwner *o){
 return o&&o->config.ports.is_owner&&o->config.ports.is_owner(o->config.ports.owner_context);
}
static void fail(StorageOwner *o,StorageOwnerState at,StorageResult error){
 o->failed_at=at;o->error=error;o->state=OWNER_FAILED;
}
static bool seed_ok(uint64_t v){return v&&v!=UINT64_MAX;}
static const char *base_of(const char *path){const char *b=strrchr(path,'/');return b?b+1:path;}
static bool same_parent(const StorageOwnerFileSpec *a,const StorageOwnerFileSpec *b){
 const char *pa=strrchr(a->path,'/'),*pb=strrchr(b->path,'/');
 if(!pa||!pb)return false;
 size_t la=(size_t)(pa-a->path),lb=(size_t)(pb-b->path);
 return la==lb&&!memcmp(a->path,b->path,la);
}
static bool name_allowed(const StorageOwner *o,const char *base,bool snapshot_metadata){
 for(unsigned k=0;k<o->name_count;k++)if(!strcmp(o->namespace_names[k].name,base))
  return o->namespace_names[k].snapshot_metadata==snapshot_metadata;
 return false;
}
/* Pure layout contract shared by init and rotation prepare. Data basenames
 * must equal the catalog descriptor names; the namespace allowlist decides
 * which basenames are ordinary and which are snapshot metadata. */
uint32_t storage_owner_next_trace;
uint32_t storage_owner_attach_trace;
static bool spec_ok(const StorageOwner *o,const StorageOwnerFileSpec *f,bool data,bool snapshot_metadata){
 if(!f->file_id||!f->order_key||!memchr(f->path,0,sizeof(f->path))||!f->path[0]||
    !memchr(f->name,0,sizeof(f->name))||!f->name[0]){storage_owner_next_trace=101;return false;}
 if(data&&(!f->catalog_id||!f->ledger_id||f->file_id==f->ledger_id||f->file_id==f->catalog_id||f->catalog_id==f->ledger_id)){storage_owner_next_trace=102;return false;}
 unsigned role=(unsigned)f->role;
 if(data&&(role<(unsigned)CATALOG_MONITOR||role>(unsigned)CATALOG_EVENT)){storage_owner_next_trace=103;return false;}
 if((unsigned)f->family>=(unsigned)RETAIN_FAMILIES){storage_owner_next_trace=104;return false;}
 size_t root=strlen(o->namespace_root);
 if(strncmp(f->path,o->namespace_root,root)||f->path[root]!='/'){storage_owner_next_trace=105;return false;}
 const char *base=base_of(f->path);
 if(!base[0]||!name_allowed(o,base,snapshot_metadata)){storage_owner_next_trace=106;return false;}
 if(data&&strcmp(base,f->name)){storage_owner_next_trace=107;return false;}return true;
}
static bool layout_ok(const StorageOwner *o,const StorageOwnerFileSpec *files,
 const StorageOwnerFileSpec *ledger,const StorageOwnerFileSpec *catalog){
 for(unsigned k=0;k<STORAGE_OWNER_FILES;k++){
  const StorageOwnerFileSpec *f=&files[k];
  if(!spec_ok(o,f,true,false))return false;
  if(f->catalog_id!=catalog->file_id||f->ledger_id!=ledger->file_id)return false;
  for(unsigned j=0;j<k;j++)if(f->file_id==files[j].file_id||!strcmp(f->path,files[j].path))return false;
  if(f->file_id==ledger->file_id||f->file_id==catalog->file_id){storage_owner_next_trace=201;return false;}
  if(!same_parent(f,ledger)||!same_parent(f,catalog)){storage_owner_next_trace=202;return false;}
 }
 if(!spec_ok(o,ledger,false,false)||!spec_ok(o,catalog,false,true))return false;
 if(ledger->file_id==catalog->file_id||!same_parent(ledger,catalog)){storage_owner_next_trace=203;return false;}
 return true;
}
static void handle_spec(const StorageOwner *o,uint64_t catalog_id,uint64_t ledger_id,
 const StorageOwnerFileSpec *f,unsigned k,StorageHandleSpec *hs){
 memset(hs,0,sizeof(*hs));
 hs->session_id=o->config.session_id;hs->file_id=f->file_id;
 hs->catalog_id=catalog_id;hs->ledger_id=ledger_id;hs->order_key=f->order_key;
 hs->kind=k<STORAGE_OWNER_FILES?HANDLE_DATA:HANDLE_AUXILIARY;hs->family=f->family;
 memcpy(hs->path,f->path,sizeof(hs->path));
}
/* Registers one group's handles in data,data,ledger,catalog order. The slot
 * indices are deterministic because registration runs on a fresh cache and
 * rotation always forgets exactly the alternate group first. */
static bool register_layout(StorageOwner *o,unsigned g,const StorageOwnerFileSpec *files,
 const StorageOwnerFileSpec *ledger,const StorageOwnerFileSpec *catalog){
 StorageOwnerGroup *w=&o->workspace.groups[g];
 for(unsigned k=0;k<STORAGE_OWNER_GROUP_FILES;k++){
  const StorageOwnerFileSpec *f=k<STORAGE_OWNER_FILES?&files[k]:k==STORAGE_OWNER_FILES?ledger:catalog;
  StorageHandleSpec hs;handle_spec(o,catalog->file_id,ledger->file_id,f,k,&hs);
  unsigned slot=0;
  StorageResult rr=storage_file_handles_register(&o->workspace.handles,&hs,&slot);
  if(rr!=STORE_OK){storage_owner_attach_trace=40+10*g+(unsigned)rr;return false;}
  if(slot!=g*STORAGE_OWNER_GROUP_FILES+k){storage_owner_attach_trace=60+10*g+k;return false;}
  w->slots[k]=slot;
 }
 return true;
}
static void factory_config(StorageOwner *o,unsigned g,bool await_first_capture,
 const StorageOwnerFileSpec *files,const StorageOwnerFileSpec *ledger,
 const StorageOwnerFileSpec *catalog,StorageFactoryConfig *c){
 StorageOwnerGroup *w=&o->workspace.groups[g];
 memset(c,0,sizeof(*c));
 c->session_id=o->config.session_id;
 c->next_sequence=o->config.next_sequence;
 c->ledger_id=ledger->file_id;c->catalog_id=catalog->file_id;
 c->sync_us=o->config.sync_us;c->created_us=o->config.created_us;
 c->max_file_bytes=o->config.max_file_bytes;c->max_file_age_us=o->config.max_file_age_us;
 c->count=STORAGE_OWNER_FILES;c->arena=w->arena;c->pool=&w->pool;c->family=&w->family;
 c->owner=&w->family_queue;c->catalog=&w->catalog;c->verifier=&w->verifier;
 c->epoch=&w->epoch;c->engine=&w->engine;
 c->locks=o->config.locks;
 c->is_owner=o->config.ports.is_owner;c->owner_context=o->config.ports.owner_context;
 c->await_first_capture=await_first_capture;
 for(unsigned k=0;k<STORAGE_OWNER_FILES;k++){
  StorageFactoryFile *a=&c->files[k];
  a->descriptor.file_id=files[k].file_id;a->descriptor.role=files[k].role;
  a->descriptor.location=files[k].location;a->descriptor.ordinal=files[k].ordinal;
  memcpy(a->descriptor.name,files[k].name,sizeof(a->descriptor.name));
  a->binding=&w->bindings[k];a->writer=&w->writers[k];a->queue=&w->queues[k];
  a->service=&w->services[k];a->pump=&w->pumps[k];a->plan=&w->plans[k];
 }
}
/* Provision has reached METADATA: factory READY with unbound capture, services
 * created. Only now can the sink, route and segment be assembled (they require
 * initialized service queues); no backend IO happens here. */
static bool assemble_ready(StorageOwner *o){
 StorageOwnerGroup *w=&o->workspace.groups[o->group];
 StorageMultiTargets targets={
  &w->pumps[o->config.targets[0]],&w->pumps[o->config.targets[1]],
  &w->pumps[o->config.targets[2]],&w->pumps[o->config.targets[3]]};
 CaptureStorageSink view;
 if(!storage_multi_sink_init(&w->sink,o->config.session_id,o->config.multi_seed,&targets,&view))return false;
 StoragePump *pumps[STORAGE_OWNER_FILES];StorageService *services[STORAGE_OWNER_FILES];
 for(unsigned k=0;k<STORAGE_OWNER_FILES;k++){pumps[k]=&w->pumps[k];services[k]=&w->services[k];}
 if(!storage_multi_sink_set_close_pumps(&w->sink,pumps,STORAGE_OWNER_FILES))return false;
 if(!capture_storage_route_init(&o->workspace.route,o->config.session_id,&o->config.route_locks,&view))return false;
 if(o->config.attach_observers&&!o->config.attach_observers(o->config.observer_context,&w->sink,&o->workspace.route,pumps,STORAGE_OWNER_FILES,1))return false;
 if(!storage_segment_init(&w->segment,services,STORAGE_OWNER_FILES,o->config.session_id,
    o->config.segment_seed,o->config.ports.is_owner,o->config.ports.owner_context))return false;
 return storage_capture_epoch_segment(&w->capture_epoch,&w->segment)==STORE_OK;
}
static bool next_valid(StorageOwner *o,const StorageOwnerNextSpec *s){
 storage_owner_next_trace=0;
 if(!s||!seed_ok(s->segment_seed)){storage_owner_next_trace=301;return false;}
 if(s->replace_layout&&!layout_ok(o,s->files,&s->ledger,&s->catalog)){if(!storage_owner_next_trace)storage_owner_next_trace=302;return false;}
 const StorageOwnerGroupSpec *g=&o->config.groups[1-o->group];
 if(g->config_id||g->capture_id){
  const StorageFactoryConfig *c=&o->workspace.groups[o->group].factory.config;
  for(unsigned k=0;k<STORAGE_OWNER_FILES;k++){
   if(g->config_id&&c->files[k].config_id!=g->config_id){storage_owner_next_trace=303;return false;}
   if(g->capture_id&&c->files[k].capture_id!=g->capture_id){storage_owner_next_trace=304;return false;}
  }
 }
 return true;
}
/* Fill StorageRotationNext from the alternate persistent group and hand it to
 * the rotation executor. The sealed epoch token is the only source of the new
 * global sequence seed; layout/limits come from the stored next spec or config.
 * No IO: the executor creates the new files in later steps. */
static StorageResult prepare_next(StorageOwner *o){
 unsigned nxt=1-o->group;
 StorageOwnerGroup *w=&o->workspace.groups[nxt];
 StorageOwnerGroup *cur=&o->workspace.groups[o->group];
 if(o->rotation.state!=ROTATION_NEED_NEXT)return STORE_BUSY;
 if(!o->have_next)return STORE_BUSY;
 if(cur->epoch.state!=EPOCH_READY||!cur->epoch.token.valid)return STORE_DEPENDENCY;
 const StorageOwnerGroupSpec *spec=&o->config.groups[nxt];
 const StorageOwnerFileSpec *files=o->next.replace_layout?o->next.files:spec->data;
 const StorageOwnerFileSpec *ledger=o->next.replace_layout?&o->next.ledger:&spec->ledger;
 const StorageOwnerFileSpec *catalog=o->next.replace_layout?&o->next.catalog:&spec->catalog;
 if(o->next.replace_layout&&!layout_ok(o,files,ledger,catalog))return STORE_INVALID;
 for(unsigned k=0;k<STORAGE_OWNER_GROUP_FILES;k++)
  if(storage_file_handles_forget(&o->workspace.handles,w->slots[k])!=STORE_OK)return STORE_INVALID;
 if(!register_layout(o,nxt,files,ledger,catalog))return STORE_INVALID;
 StorageRotationNext *next=&o->rotation_next;
 memset(next,0,sizeof(*next));
 next->provision=&w->provision;next->epoch=&w->capture_epoch;next->factory=&w->factory;
 next->lifecycle=&w->lifecycle;next->sink=&w->sink;next->segment=&w->segment;
 factory_config(o,nxt,false,files,ledger,catalog,&next->config);
 next->config.next_sequence=cur->epoch.token.next_sequence;
 for(unsigned k=0;k<STORAGE_OWNER_FILES;k++){
  next->config.files[k].config_id=cur->factory.config.files[k].config_id;
  next->config.files[k].capture_id=cur->factory.config.files[k].capture_id;
 }
 if(o->next.sync_us)next->config.sync_us=o->next.sync_us;
 if(o->next.max_file_bytes)next->config.max_file_bytes=o->next.max_file_bytes;
 if(o->next.max_file_age_us)next->config.max_file_age_us=o->next.max_file_age_us;
 next->ops=o->config.ports.provision;
 memcpy(next->slots,w->slots,sizeof(w->slots));
 for(unsigned k=0;k<STORAGE_OWNER_TARGETS;k++)next->targets[k]=o->config.targets[k];
 next->segment_seed=o->next.segment_seed;
 next->attach_observers=o->config.attach_observers;next->observer_context=o->config.observer_context;
 return storage_rotation_prepare(&o->rotation,next);
}
static void step_rotation(StorageOwner *o,uint64_t now){
 storage_rotation_step(&o->rotation,now);
 if(o->rotation.state==ROTATION_FAILED){fail(o,o->state,o->rotation.error);return;}
 if(o->rotation.state==ROTATION_NEED_NEXT&&o->have_next){
  StorageResult r=prepare_next(o);
  if(r!=STORE_OK)fail(o,o->state,r);
  return;
 }
 if(o->rotation.state==ROTATION_DONE){
  o->group=1-o->group;
  StorageOwnerGroup *g=&o->workspace.groups[o->group];
  /* The rotation executor installed its latched pause proof on the new epoch;
   * after a successful RESUME the caller's live source-off proof is current. */
  g->capture_epoch.producer_stopped=o->config.ports.producer_stopped;
  g->capture_epoch.proof_context=o->config.ports.proof_context;
  o->have_next=false;memset(&o->next,0,sizeof(o->next));
  o->state=OWNER_RUNNING;
 }
}
bool storage_owner_prepare(StorageOwner *o,const StorageOwnerConfig *c){
 if(!o||!c)return false;
 memset(o,0,sizeof(*o));
 o->config=*c;
 o->state=OWNER_FAILED;o->error=STORE_INVALID;o->failed_at=OWNER_PROVISION;
 if(!c->root||strlen(c->root)>=sizeof(o->namespace_root))return false;
 memcpy(o->namespace_root,c->root,strlen(c->root)+1);
 if(!c->names||!c->name_count||c->name_count>STORAGE_OWNER_NAMES)return false;
 memcpy(o->namespace_names,c->names,c->name_count*sizeof(*c->names));
 o->name_count=c->name_count;
 const StorageOwnerPorts *p=&c->ports;
 if(!c->session_id||!seed_ok(c->next_sequence)||!seed_ok(c->segment_seed)||!seed_ok(c->multi_seed))return false;
 if(!c->sync_us||!c->max_file_bytes||!c->max_file_age_us||!c->owner_us)return false;
 if(!p->is_owner||!p->is_owner(p->owner_context)||!p->producer_stopped)return false;
 if(!p->cold.attach||!p->cold.copy||!p->cold.ack)return false;
 if(!p->rotation.submit||!p->rotation.copy||!p->rotation.ack)return false;
 for(unsigned k=0;k<STORAGE_OWNER_TARGETS;k++)if(c->targets[k]>=STORAGE_OWNER_FILES)return false;
 for(unsigned g=0;g<STORAGE_OWNER_GROUPS;g++){
  const StorageOwnerGroupSpec *s=&c->groups[g];
  if(!layout_ok(o,s->data,&s->ledger,&s->catalog))return false;
  if(!g&&(s->config_id||s->capture_id))return false;
 }
 if(!storage_namespace_init(&o->workspace.space,o->namespace_root,1,p->is_owner,p->owner_context))return false;
 o->group=0;o->have_time=false;o->last_now=0;o->have_next=false;
 o->state=OWNER_PREPARED;o->error=STORE_OK;o->failed_at=OWNER_PROVISION;
 return true;
}
bool storage_owner_attach_ports(StorageOwner *o,const StorageOwnerPorts *p){
 if(!o||!p||o->state!=OWNER_PREPARED)return false;
 if(!p->is_owner||!p->is_owner(p->owner_context)||!p->producer_stopped)return false;
 if(p->is_owner!=o->config.ports.is_owner||p->owner_context!=o->config.ports.owner_context)return false;
 if(p->producer_stopped!=o->config.ports.producer_stopped||p->proof_context!=o->config.ports.proof_context)return false;
 if(!p->files.is_owner||!p->files.open||!p->files.close||!p->files.remove||!p->files.sync_media)return false;
 if(!p->files.is_owner(p->files.context))return false;
 if(!p->provision.validate||!p->provision.create||!p->provision.sync_media)return false;
 o->config.ports=*p;
 storage_owner_attach_trace=1;
 StorageFileOps files=p->files;
 if(!storage_file_handles_init(&o->workspace.handles,&files))return false;
 storage_owner_attach_trace=2;
 for(unsigned g=0;g<STORAGE_OWNER_GROUPS;g++){
  const StorageOwnerGroupSpec *s=&o->config.groups[g];
  if(!register_layout(o,g,s->data,&s->ledger,&s->catalog)){if(storage_owner_attach_trace<40)storage_owner_attach_trace=20+g;return false;}
 }
 StorageOwnerGroup *w=&o->workspace.groups[0];
 StorageFactoryConfig *cfg=&o->rotation_next.config;
 factory_config(o,0,true,o->config.groups[0].data,&o->config.groups[0].ledger,&o->config.groups[0].catalog,cfg);
 storage_owner_attach_trace=3;
 if(!storage_capture_epoch_init(&w->capture_epoch,&o->workspace.handles,&w->factory,&w->lifecycle,
    &o->workspace.space,w->slots,STORAGE_OWNER_FILES,p->producer_stopped,p->proof_context,&cfg->hooks))return false;
 storage_owner_attach_trace=4;
 if(!storage_provision_init(&w->provision,&p->provision,&o->workspace.handles,&o->workspace.space,
    &w->factory,&w->lifecycle,cfg,w->slots))return false;
 storage_owner_attach_trace=5;
 if(!storage_rotation_init(&o->rotation,&p->rotation,p->is_owner,p->owner_context))return false;
 storage_owner_attach_trace=6;
 memset(&o->rotation_next,0,sizeof(o->rotation_next));
 /* Caller strings/allowlist are consumed; keep no dangling pointers. */
 o->config.root=NULL;o->config.names=NULL;o->config.name_count=0;
 o->state=OWNER_PROVISION;o->error=STORE_OK;o->failed_at=OWNER_PROVISION;
 return true;
}
bool storage_owner_init(StorageOwner *o,const StorageOwnerConfig *c){
 if(!storage_owner_prepare(o,c))return false;
 if(!storage_owner_attach_ports(o,&c->ports)){o->state=OWNER_FAILED;o->error=STORE_INVALID;return false;}
 return true;
}
StorageResult storage_owner_begin_cold(StorageOwner *o,const StorageSessionFacts *facts){
 if(!o||!facts||!owned(o))return STORE_INVALID;
 if(o->state!=OWNER_READY)return STORE_BUSY;
 StorageOwnerGroup *g=&o->workspace.groups[o->group];
 if(g->provision.state!=PROVISION_METADATA||g->segment.state!=SEGMENT_IDLE)return STORE_BUSY;
 if(!storage_cold_start_init(&o->cold,&g->capture_epoch,&o->workspace.route,&g->sink,
    &o->config.ports.cold,facts)){fail(o,o->state,STORE_INVALID);return STORE_INVALID;}
 o->state=OWNER_COLD;
 return STORE_OK;
}
StorageResult storage_owner_segment(StorageOwner *o,const StorageConfigFacts *cfg,const StorageCaptureFacts *clock){
 if(!o||!cfg||!clock||!owned(o))return STORE_INVALID;
 if(o->state!=OWNER_WAIT_FACTS)return STORE_BUSY;
 uint64_t token=0;
 StorageResult r=storage_cold_start_segment(&o->cold,cfg,clock,&token);
 if(r==STORE_OK)o->state=OWNER_COLD;
 else if(r!=STORE_BUSY)fail(o,o->state,r);
 return r;
}
void storage_owner_step(StorageOwner *o,uint64_t now_us){
 if(!o||o->state==OWNER_FAILED||o->state==OWNER_STOPPED)return;
 if(!owned(o))return;
 if(o->have_time&&now_us<o->last_now){fail(o,o->state,STORE_INVALID);return;}
 o->have_time=true;o->last_now=now_us;
 StorageOwnerGroup *g=&o->workspace.groups[o->group];
 switch(o->state){
 case OWNER_PROVISION:
  storage_provision_step(&g->provision,now_us);
  if(g->provision.state==PROVISION_FAILED){fail(o,o->state,g->provision.error);return;}
  if(g->provision.state==PROVISION_METADATA){
   if(!assemble_ready(o)){fail(o,o->state,STORE_INVALID);return;}
   o->state=OWNER_READY;
  }
  return;
 case OWNER_COLD:
  storage_cold_start_step(&o->cold,now_us);
  if(o->cold.state==COLD_FAILED){fail(o,o->state,o->cold.error);return;}
  if(o->cold.state==COLD_DONE)o->state=OWNER_RUNNING;
  else if(o->cold.state==COLD_WAIT_FACTS)o->state=OWNER_WAIT_FACTS;
  return;
 case OWNER_RUNNING:
  if(storage_event_roll_active(&o->event_roll)){
   capture_storage_route_step(&o->workspace.route);
   CaptureRouteHealth health;if(capture_storage_route_health(&o->workspace.route,&health)&&health.consumer.blocked){fail(o,o->state,health.consumer.error);return;}
   storage_event_roll_step(&o->event_roll,now_us);
   if(o->event_roll.state==EVENT_ROLL_FAILED){fail(o,o->state,o->event_roll.error);return;}
   if(o->event_roll.state==EVENT_ROLL_DONE){
    unsigned k=o->event_roll.file;StorageOwnerFileSpec *spec=&o->config.groups[o->group].data[k];
    spec->file_id=o->event_roll.next.file_id;spec->order_key=o->event_roll.spec.order_key;spec->location=o->event_roll.next.location;spec->ordinal=o->event_roll.next.ordinal;memcpy(spec->path,o->event_roll.spec.path,sizeof(spec->path));
   }
   return;
  }
  storage_capture_epoch_step(&g->capture_epoch,now_us);
  if(g->capture_epoch.state==CAPTURE_EPOCH_FAILED){fail(o,o->state,g->capture_epoch.error);return;}
  return;
 case OWNER_STOPPING:
  storage_capture_epoch_step(&g->capture_epoch,now_us);
  if(g->capture_epoch.state==CAPTURE_EPOCH_FAILED){fail(o,o->state,g->capture_epoch.error);return;}
  if(g->capture_epoch.state==CAPTURE_EPOCH_HANDOFF){
   StorageResult r=storage_capture_epoch_finish(&g->capture_epoch);
   if(r==STORE_OK)o->state=OWNER_STOPPED;
   else if(r!=STORE_BUSY)fail(o,o->state,r);
  }
  return;
 case OWNER_ROTATING:step_rotation(o,now_us);return;
 default:return;
 }
}
StorageResult storage_owner_request_stop(StorageOwner *o){
 if(!o||!owned(o))return STORE_INVALID;
 if(o->state!=OWNER_RUNNING||storage_event_roll_active(&o->event_roll))return STORE_BUSY;
 StorageOwnerGroup *g=&o->workspace.groups[o->group];
 StorageResult r=storage_capture_epoch_stop(&g->capture_epoch);
 if(r==STORE_OK)o->state=OWNER_STOPPING;
 return r;
}
StorageResult storage_owner_begin_rotation(StorageOwner *o,const StorageOwnerNextSpec *spec){
 if(!o||!spec||!owned(o))return STORE_INVALID;
 if(o->state==OWNER_RUNNING&&!storage_event_roll_active(&o->event_roll)){
  StorageOwnerGroup *g=&o->workspace.groups[o->group];
  if(o->rotation.state!=ROTATION_IDLE&&o->rotation.state!=ROTATION_DONE)return STORE_BUSY;
  if(!next_valid(o,spec))return STORE_INVALID;
  StorageResult r=storage_rotation_begin(&o->rotation,&g->capture_epoch);
  if(r!=STORE_OK)return r;
  o->next=*spec;o->have_next=true;o->state=OWNER_ROTATING;
  return STORE_OK;
 }
 return STORE_BUSY;
}
StorageOwnerState storage_owner_state(const StorageOwner *o){return o?o->state:OWNER_FAILED;}
StorageResult storage_owner_error(const StorageOwner *o){return o?o->error:STORE_INVALID;}
CaptureStorageRoute *storage_owner_route(StorageOwner *o){return o?&o->workspace.route:NULL;}
StorageResult storage_owner_begin_event_roll(StorageOwner *o,const StorageEventRollOps *ops,uint64_t ordinal,uint64_t created){
 if(!o||!ops||!owned(o))return STORE_INVALID;
 if(o->state!=OWNER_RUNNING)return STORE_BUSY;
 StorageOwnerGroup *g=&o->workspace.groups[o->group];
 if(g->segment.state!=SEGMENT_IDLE&&g->segment.state!=SEGMENT_SYNCED)return STORE_BUSY;
 unsigned event=STORAGE_OWNER_FILES;for(unsigned k=0;k<STORAGE_OWNER_FILES;k++)if(g->factory.config.files[k].descriptor.role==CATALOG_EVENT){if(event!=STORAGE_OWNER_FILES)return STORE_INVALID;event=k;}
 if(event==STORAGE_OWNER_FILES)return STORE_INVALID;
 for(unsigned k=0;k<STORAGE_OWNER_TARGETS;k++)if(o->config.targets[k]==event)return STORE_INVALID;
 const char *catalog=o->workspace.handles.files[g->slots[STORAGE_OWNER_FILES+1]].spec.path;
 return storage_event_roll_request(&o->event_roll,&g->factory,&o->workspace.handles,&o->workspace.space,ops,event,g->slots[event],catalog,ordinal,created);
}
