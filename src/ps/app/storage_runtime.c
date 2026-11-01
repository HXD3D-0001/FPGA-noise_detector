#include "storage_runtime.h"
#include "storage_owner_freertos.h"
#include "summary_runtime.h"
#include "event_runtime.h"
#include "../storage/storage_namespace.h"
#include "../storage/fatfs_backend.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "xiltimer.h"
#include "../time/timer_scale.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#if !defined(FREERTOS_BSP)
#error Require FreeRTOS BSP
#endif
enum { RT_STACK_WORDS=7168, RT_MAX_EPOCH=65535/STORAGE_OWNER_GROUP_FILES-1, RT_STOP_WAIT_TICKS=2000, RT_DIAGNOSTIC_WAIT_TICKS=10 };
typedef struct {uint32_t kind;uint64_t session;} RtMsg;
typedef enum {RT_CMD_SESSION=1,RT_CMD_STOP=2} RtCmd;
static StorageOwner rt_owner __attribute__((section(".lfn_data.storage_runtime"),aligned(64),used));
static SummaryRuntime rt_summary __attribute__((section(".lfn_data.storage_runtime"),aligned(64),used));
static PcmHistory rt_history __attribute__((section(".lfn_data.storage_runtime"),aligned(64),used));
static EventRuntime rt_events __attribute__((section(".lfn_data.storage_runtime"),aligned(64),used));
static EventFeed rt_event_feed __attribute__((section(".lfn_data.storage_runtime"),aligned(64),used));
static MarkerRuntime rt_markers __attribute__((section(".lfn_data.storage_runtime"),aligned(64),used));
static StorageOwnerFreertos rt_fs __attribute__((section(".lfn_data.storage_runtime"),aligned(64),used));
static LfnFatFsVolume rt_volume;
static TaskHandle_t rt_task;static QueueHandle_t rt_queue;
static SemaphoreHandle_t rt_mutex;
static atomic_bool rt_started,rt_active,rt_closed,rt_failed;
static atomic_bool rt_stop,rt_capture_close,rt_ready,rt_restart,rt_rotate_request;
static _Atomic unsigned rt_state,rt_phase,rt_error,rt_stage;
static _Atomic uint64_t rt_session,rt_steps;
static _Atomic uint64_t rt_max_bytes=UINT64_C(32)*1024*1024,rt_max_age=UINT64_C(1800)*1000000;
static _Atomic unsigned rt_rotations,rt_epoch;
static _Atomic uint64_t rt_generation,rt_active_since;
static _Atomic unsigned rt_rot_stage,rt_rot_error;
static char rt_root[16];
static StorageOwnerNamespaceName rt_names[STORAGE_OWNER_HANDLES];static LfnFatFsNamespaceName rt_fs_names[STORAGE_OWNER_HANDLES];
static StorageOwnerGroupSpec rt_groups[STORAGE_OWNER_GROUPS];
static StorageOwnerNextSpec rt_next;
static PlCaptureConfig rt_cfg;
static atomic_bool rt_source_off;static atomic_bool rt_capture_stopped=ATOMIC_VAR_INIT(true);
static atomic_bool rt_auto_rotate;
static atomic_bool rt_restart_pending;static PlCaptureConfig rt_restart_cfg;
/* 0: no request, 1: wait for rotation, 2: owner holds the running epoch. */
static _Atomic unsigned rt_stop_intent;
static uint64_t rt_restart_since,rt_segment_token;
static CaptureRouteFacts rt_last_facts;
static StorageRuntimeDiagnostics rt_diagnostics;
static bool rt_is_owner(void *c){(void)c;return rt_task&&xTaskGetCurrentTaskHandle()==rt_task;}
static bool rt_mutex_lock(void *c){(void)c;return rt_mutex&&xSemaphoreTake(rt_mutex,0)==pdTRUE;}
static void rt_mutex_unlock(void *c){(void)c;if(rt_mutex)(void)xSemaphoreGive(rt_mutex);}
static bool rt_publish_diagnostics(void){
 StorageRuntimeDiagnostics d={0};CaptureRouteHealth rh;
 if(capture_storage_route_health(&rt_owner.workspace.route,&rh)){
  d.valid=true;d.blocked=rh.consumer.blocked;d.error_kind=rh.consumer.error_kind;
  d.error=(unsigned)rh.consumer.error;d.dropped=rh.source.dropped;
  d.overflow=rh.source.overflow;d.accepted=rh.consumer.accepted;
 }
 d.step=atomic_load_explicit(&rt_steps,memory_order_relaxed);
 d.summary_count=rt_summary.summary_count;d.summary_capture_revision=rt_summary.capture_definition.revision;d.summary_error=(unsigned)rt_summary.error;
 marker_runtime_health(&rt_markers,&d.markers);d.markers.unlinked=rt_summary.marker_unlinked;
 for(unsigned f=0;f<4;f++)for(unsigned k=0;k<STORAGE_RECEIPTS;k++)if(rt_summary.pending[f][k].used)d.summary_pending++;
 StorageOwnerGroup *g=&rt_owner.workspace.groups[rt_owner.group];
 for(unsigned i=0;i<STORAGE_OWNER_FILES;i++){
  StorageRuntimeFileHealth *f=&d.files[i];StorageService *s=&g->services[i];
  f->bytes=g->writers[i].file_bytes;f->accepted=s->health.accepted;
  f->synced=s->health.synced;f->invalid=s->health.invalid;
  f->service_fatal=s->health.fatal;f->pump_fatal=g->pumps[i].health.fatal;
  f->unsaved=s->unsaved.count;f->unsaved_overflow=s->unsaved.overflow;
  StorageQueueHealth q;
  if(g->queues[i].meta){
   if(storage_queue_health(&g->queues[i],&q)==STORE_OK){f->queue_valid=true;f->live_jobs=q.live_jobs;}
   else d.valid=false;
  }
 }
 /* Only this task reads owner internals; UART copies this bounded snapshot.
  * A busy lock preserves the previous sample and its explicit step number. */
 if(!rt_mutex_lock(NULL))return false;
 rt_diagnostics=d;rt_mutex_unlock(NULL);return d.valid;
}
static uint64_t now_us(void){XTime n;XTime_GetTime(&n);return lfn_timer_ticks_us(n,COUNTS_PER_SECOND);}
/* Both callbacks execute only in the capture task. No service or FatFs access. */
static void rt_detect_energy(void*c,const LfnServiceIdentity*id,const CaptureEnergyMessage*m){
 (void)c;if(!atomic_load_explicit(&rt_ready,memory_order_acquire)||id->session_id!=atomic_load_explicit(&rt_session,memory_order_acquire))return;
 if(event_feed_energy(&rt_event_feed,id,m,now_us())!=EVENT_OK)atomic_store_explicit(&rt_failed,true,memory_order_release);
}
static void rt_detect_snapshot(void*c,const LfnServiceSnapshot*s){
 (void)c;if(!atomic_load_explicit(&rt_ready,memory_order_acquire)||!s->prepared_valid||s->prepared.session_id!=atomic_load_explicit(&rt_session,memory_order_acquire))return;
 if(s->analysis.has_spectrum&&s->analysis.spectrum.valid&&s->analysis.spectrum.flags==LFN_VALID)
  (void)event_feed_spectrum(&rt_event_feed,&s->analysis.spectrum,s->published_us);
 if(s->capture_valid&&(s->runtime_state==CR_STOPPED||s->runtime_state==CR_FAILED||s->runtime_state==CR_QUARANTINED))
  (void)event_feed_finish(&rt_event_feed,s->runtime_state==CR_STOPPED?EVENT_STOP:EVENT_LOSS,s->published_us);
 if(event_feed_error(&rt_event_feed)!=EVENT_OK)atomic_store_explicit(&rt_failed,true,memory_order_release);
}
bool lfn_storage_runtime_observe(LfnServiceBridge*b){
 LfnServiceProducerObserver observer={NULL,rt_detect_energy,rt_detect_snapshot};
 return lfn_service_producer_observer(b,&observer);
}
static bool rt_proof(void *c){(void)c;return atomic_load_explicit(&rt_capture_stopped,memory_order_acquire);}
static void rt_schedule_delay(unsigned *seal_batch){
 /* Only the stopped producer's seal verification may use a short batch.
  * Recheck every iteration so state/group changes immediately restore normal
  * pacing. Each underlying owner/engine step keeps its existing IO bound. */
 if((rt_owner.state==OWNER_STOPPING||rt_owner.state==OWNER_ROTATING)&&
    rt_owner.workspace.groups[rt_owner.group].engine.state==ENGINE_ROTATING&&rt_proof(NULL)){
  if(++(*seal_batch)<32u)return;
 }
 *seal_batch=0;vTaskDelay(1);
}
static StorageIoResult rt_media(void *c){(void)c;return STORE_IO_OK;}
static StorageIoResult rt_event_mkdir(void*c,const char*path){(void)c;return lfn_fatfs_mkdir(&rt_volume,path,true);}
static StorageResult rt_event_create(void*c,StorageFileHandles*h,unsigned slot){(void)c;return lfn_fatfs_file_port_create(&rt_fs.files,h,slot);}
static StorageIoResult rt_event_media(void*c,StorageFileHandles*h){(void)c;return h->ops.sync_media(h->ops.context);}
static bool rt_event_observer(void*c,StorageService*s,uint64_t file){(void)c;return event_runtime_bind_file(&rt_events,s,file);}
static StorageResult rt_event_begin(void*c,uint64_t ordinal,uint64_t now){
 (void)c;StorageEventRollOps ops={NULL,rt_event_mkdir,rt_event_create,rt_event_media,rt_event_observer};
 return storage_owner_begin_event_roll(&rt_owner,&ops,ordinal,now);
}
static bool rt_attach_observers(void*c,StorageMultiSink*s,CaptureStorageRoute*r,StoragePump*const*p,unsigned n,uint64_t generation){
 (void)c;
 return event_runtime_attach(&rt_events,s,r,p,n,generation)&&marker_runtime_attach(&rt_markers,s,p[2]->service);
}
static StorageResult rt_event_status(void*c,uint64_t*file){(void)c;return storage_event_roll_status(&rt_owner.event_roll,file);}
/* Unique low-word tags per epoch. Paths are NEW, never reused when the
 * two RAM workspaces alternate. The namespace still checks each basename. */
static bool rt_layout(StorageOwnerGroupSpec *group,const char *root,uint64_t session,unsigned epoch){
 static const char *suffix[STORAGE_OWNER_GROUP_FILES]={"MON.LFR","RAW.LFR",
#if LFN_STORAGE_OWNER_FILES == 4
 "SUM.LFR","EVT.LFR",
#endif
 "IDX.SFL","DIR.SFC"};
 if(epoch>RT_MAX_EPOCH)return false;
 memset(group,0,sizeof(*group));
 uint64_t base=(session&UINT64_C(0xFFFFFFFFFFFF))<<16;
 for(unsigned k=0;k<STORAGE_OWNER_GROUP_FILES;k++){
  StorageOwnerFileSpec *s=k<STORAGE_OWNER_FILES?&group->data[k]:k==STORAGE_OWNER_FILES?&group->ledger:&group->catalog;
  s->file_id=base|(uint64_t)(epoch*STORAGE_OWNER_GROUP_FILES+k+1);s->order_key=(uint64_t)epoch*STORAGE_OWNER_GROUP_FILES+k+1;
  s->role=k==STORAGE_OWNER_PSD?CATALOG_RAW:CATALOG_MONITOR;
  s->family=k<STORAGE_OWNER_FILES?RETAIN_PSD:RETAIN_METADATA;
#if LFN_STORAGE_OWNER_FILES == 4
  if(k==STORAGE_OWNER_SUMMARY)s->family=RETAIN_SUMMARY;
  if(k==STORAGE_OWNER_EVENT){s->role=CATALOG_EVENT;s->family=RETAIN_PCM;}
#endif
  (void)snprintf(s->name,sizeof(s->name),"%c%s",(epoch&1)?'B':'A',suffix[k]);
  if(epoch)(void)snprintf(s->path,sizeof(s->path),"%s/E%06X/%s",root,epoch,s->name);
  else (void)snprintf(s->path,sizeof(s->path),"%s/%s",root,s->name);
 }
 for(unsigned k=0;k<STORAGE_OWNER_FILES;k++){group->data[k].ledger_id=group->ledger.file_id;group->data[k].catalog_id=group->catalog.file_id;}
 return true;
}
static void rt_build_configs(StorageOwnerConfig *oc,StorageOwnerFreertosConfig *fc){
 static const char *names[STORAGE_OWNER_HANDLES]={"AMON.LFR","ARAW.LFR",
#if LFN_STORAGE_OWNER_FILES == 4
 "ASUM.LFR","AEVT.LFR",
#endif
 "AIDX.SFL","ADIR.SFC","BMON.LFR","BRAW.LFR",
#if LFN_STORAGE_OWNER_FILES == 4
 "BSUM.LFR","BEVT.LFR",
#endif
 "BIDX.SFL","BDIR.SFC"};
 memset(oc,0,sizeof(*oc));memset(fc,0,sizeof(*fc));
 for(unsigned k=0;k<STORAGE_OWNER_HANDLES;k++){
  strncpy(rt_names[k].name,names[k],sizeof(rt_names[k].name)-1);
  rt_names[k].snapshot_metadata=(k%STORAGE_OWNER_GROUP_FILES)==STORAGE_OWNER_GROUP_FILES-1;
  strncpy(rt_fs_names[k].name,names[k],sizeof(rt_fs_names[k].name)-1);
  rt_fs_names[k].snapshot_metadata=rt_names[k].snapshot_metadata;
 }
 uint64_t session=atomic_load_explicit(&rt_session,memory_order_acquire);
 for(unsigned g=0;g<STORAGE_OWNER_GROUPS;g++)(void)rt_layout(&rt_groups[g],rt_root,session,g);
 StorageOwnerPorts ports;memset(&ports,0,sizeof(ports));
 (void)lfn_capture_image_cold_port(&ports.cold);
 (void)lfn_capture_image_rotation_port(&ports.rotation);
 ports.is_owner=rt_is_owner;ports.owner_context=NULL;
 ports.producer_stopped=rt_proof;ports.proof_context=NULL;
 oc->session_id=session;oc->next_sequence=UINT64_C(12345);oc->segment_seed=session^UINT64_C(0x5A5A5A5A);
 oc->multi_seed=session^UINT64_C(0xA5A5A5A5);oc->sync_us=UINT64_C(1000000);
 oc->max_file_bytes=atomic_load(&rt_max_bytes);oc->max_file_age_us=atomic_load(&rt_max_age);
 oc->created_us=now_us();oc->owner_us=100000;oc->root=rt_root;
 oc->names=rt_names;oc->name_count=STORAGE_OWNER_HANDLES;oc->targets[0]=0;oc->targets[1]=1;oc->targets[2]=0;oc->targets[3]=1;
 oc->groups[0]=rt_groups[0];oc->groups[1]=rt_groups[1];oc->ports=ports;
 oc->locks=(StorageQueueLocks){NULL,rt_mutex_lock,rt_mutex_unlock};
 oc->attach_observers=rt_attach_observers;oc->observer_context=NULL;
 oc->route_locks=(StorageQueueLocks){NULL,rt_mutex_lock,rt_mutex_unlock};
 *fc=(StorageOwnerFreertosConfig){&rt_volume,"0:",rt_root,rt_fs_names,STORAGE_OWNER_HANDLES,rt_media,NULL,
   rt_is_owner,NULL,rt_proof,NULL,ports.cold,ports.rotation};
}
static bool rt_make_dir(void){
 atomic_store_explicit(&rt_stage,1u,memory_order_release);
 if(!lfn_fatfs_volume_init(&rt_volume,"0:",rt_is_owner,NULL))return false;
 atomic_store_explicit(&rt_stage,2u,memory_order_release);
 if(lfn_fatfs_mount(&rt_volume)!=STORE_IO_OK)return false;
 atomic_store_explicit(&rt_stage,3u,memory_order_release);
 bool ok=lfn_fatfs_mkdir(&rt_volume,rt_root,false)==STORE_IO_OK;
 atomic_store_explicit(&rt_stage,4u,memory_order_release);
 if(lfn_fatfs_unmount(&rt_volume)!=STORE_IO_OK)ok=false;
 return ok;
}
static void rt_latch_failure(void){
 atomic_store_explicit(&rt_failed,true,memory_order_release);
 atomic_store_explicit(&rt_active,false,memory_order_release);
 atomic_store_explicit(&rt_closed,false,memory_order_release);
 atomic_store_explicit(&rt_ready,false,memory_order_release);
 atomic_store_explicit(&rt_restart,false,memory_order_release);
 atomic_store_explicit(&rt_restart_pending,false,memory_order_release);
 atomic_store_explicit(&rt_rotate_request,false,memory_order_release);
}
static StorageResult rt_finish_session(void){
 /* A failed owner retains its tickets, handles and unsaved data for explicit
  * recovery. A blocked route cannot prove a successful drain or detach. */
 if(storage_owner_state(&rt_owner)==OWNER_FAILED){
  rt_latch_failure(); /* Capture cleanup must not wait for diagnostics. */
  for(unsigned k=0;k<RT_DIAGNOSTIC_WAIT_TICKS;k++){
   if(rt_publish_diagnostics())break;
   vTaskDelay(1);
  }
  return STORE_INVALID;
 }
 bool drained=false;
 for(unsigned k=0;k<RT_STOP_WAIT_TICKS;k++){
  CaptureRouteHealth rh;
  if(capture_storage_route_health(&rt_owner.workspace.route,&rh)&&rh.consumer.handoff_drained&&!rh.consumer.blocked){drained=true;break;}
  capture_storage_route_step(&rt_owner.workspace.route);
  vTaskDelay(1);
 }
 if(!drained||lfn_storage_owner_freertos_unmount(&rt_fs)!=STORE_IO_OK){
  atomic_store_explicit(&rt_stage,drained?951u:950u,memory_order_release);
  rt_latch_failure();return STORE_INVALID;
 }
 /* Publish all final file counters before closed becomes visible. Failed
  * diagnostic locking leaves an explicitly stale snapshot, never invented IO. */
 for(unsigned k=0;k<RT_DIAGNOSTIC_WAIT_TICKS;k++){
  if(rt_publish_diagnostics())break;
  vTaskDelay(1);
 }
 atomic_store_explicit(&rt_ready,false,memory_order_release);
 atomic_store_explicit(&rt_active,false,memory_order_release);
 atomic_store_explicit(&rt_closed,true,memory_order_release);
 return STORE_OK;
}
static unsigned rt_capture_code(const CaptureRouteFacts *q,const PlCaptureConfig *cfg){
 const PlContractSnapshot *a=&q->active;const PlCaptureSnapshot *s=&q->started;
 unsigned mask=cfg->mode?CAPTURE_RAW_MASK:CAPTURE_MONITOR_MASK;
 if(!a->seq)return 1;
 if(a->kind!=PL_CONTRACT_ACTIVE)return 2;
 if(a->age_ticks>100)return 3;
 if(!a->plan.session_id)return 4;
 if(!a->plan.capture_id)return 5;
 if(a->plan.config_id!=cfg->config_id)return 6;
 if(a->plan.raw_mode!=(cfg->mode!=0))return 7;
 if(a->mode!=cfg->mode)return 8;
 if(a->source!=cfg->source)return 9;
 if(a->channel!=cfg->channel)return 10;
 if(a->pattern!=cfg->pattern)return 11;
 if(a->bound_mask!=mask)return 12;
 if(!s->seq)return 13;
 if(s->age_ticks>100)return 14;
 if(s->capture_id!=a->plan.capture_id)return 15;
 if(s->active_config_id!=cfg->config_id)return 16;
 if(s->configured.config_id!=cfg->config_id||s->configured.mode!=cfg->mode||s->configured.source!=cfg->source||
    s->configured.channel!=cfg->channel||s->configured.pattern!=cfg->pattern)return 17;
 if((s->flags&(PL_CAPTURE_F_RUNNING|PL_CAPTURE_F_POSITION_VALID))!=(PL_CAPTURE_F_RUNNING|PL_CAPTURE_F_POSITION_VALID))return 18;
 if(!!(s->flags&PL_CAPTURE_F_TEST)!=(cfg->source==PL_CAPTURE_TEST))return 19;
 for(unsigned k=0;k<4;k++)if(mask&(1u<<k)){const CaptureAnchor *x=&a->anchors[k];
  if(x->session_id!=a->plan.session_id||x->capture_id!=a->plan.capture_id||x->config_id!=cfg->config_id||
     x->stream!=k+1||x->first_seq!=a->plan.first_seq[k])return 20+k;}
 return 0;
}
static StorageResult rt_begin_segment(const CaptureRouteFacts *facts,bool cold,uint64_t *token){
 static const char dev[]="LFN_AX7021";
 static const char hash[]="0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
 StorageConfigFacts cfg;memset(&cfg,0,sizeof(cfg));
 cfg.device_id=dev;cfg.config_hash=hash;cfg.hardware_id=facts->identity.config.source==PL_CAPTURE_ADC?"AX7021":"AX7021_PRE_ADC";
 cfg.gain_setting_id="UNVERIFIED";
 cfg.adc_id=facts->identity.config.source==PL_CAPTURE_ADC?"PCM1808_HW935":"NONE";
 cfg.microphone_id=facts->identity.config.source==PL_CAPTURE_ADC?"MAX4466":"NONE";
 cfg.point_pose_id="NONE";cfg.band_definition_id="LFN_THIRD_OCTAVE";
 cfg.session_id=facts->identity.session_id;cfg.config=facts->identity.config;
 cfg.event=(StorageEventFacts){6,3,60,5,20,5e-10,3,16};
 StorageCaptureFacts clock;memset(&clock,0,sizeof(clock));
 if(facts->time_pair.status!=CAPTURE_TIME_CONFIRMED_NOMINAL)return STORE_DEPENDENCY;
 uint64_t first_raw=facts->first_raw[cfg.config.mode?2:3];
 clock.device_id=dev;clock.config=cfg.config;clock.max_age_ticks=UINT32_MAX;clock.revision=1;
 clock.schema_version=2;clock.time_pair=facts->time_pair;
 clock.started=facts->started;clock.active=facts->active;
 clock.first_valid_raw=first_raw;
 atomic_store_explicit(&rt_stage,700u+rt_capture_code(facts,&cfg.config),memory_order_release);
 if(cold)return storage_owner_segment(&rt_owner,&cfg,&clock);
 return storage_segment_begin(&rt_owner.workspace.groups[rt_owner.group].segment,facts,&cfg,&clock,token);
}
static void rt_restart_failure(unsigned stage,StorageResult error){
 rt_owner.failed_at=rt_owner.state;rt_owner.state=OWNER_FAILED;rt_owner.error=error;
 atomic_store_explicit(&rt_state,OWNER_FAILED,memory_order_release);
 atomic_store_explicit(&rt_stage,stage,memory_order_release);
 atomic_store_explicit(&rt_error,(unsigned)error,memory_order_release);
 rt_latch_failure();
}
static bool rt_restart_expired(uint64_t current){
 return current<rt_restart_since||current-rt_restart_since>=UINT64_C(10000000);
}
static bool rt_epoch_dir(unsigned epoch){
 char rel[24];
 if(!epoch)return true;
 (void)snprintf(rel,sizeof(rel),"%s/E%06X",rt_root,epoch);
 uint64_t token=0;
 if(storage_namespace_write_begin(&rt_owner.workspace.space,&token)!=STORE_OK)return false;
 bool ok=lfn_fatfs_mkdir(&rt_volume,rel,true)==STORE_IO_OK;
 if(storage_namespace_write_end(&rt_owner.workspace.space,token)!=STORE_OK)ok=false;
 return ok;
}
static bool rt_start_rotation(void){
 unsigned next=(unsigned)atomic_load_explicit(&rt_epoch,memory_order_acquire)+1;
 if(next>RT_MAX_EPOCH){atomic_store_explicit(&rt_rot_stage,940u,memory_order_release);return false;}
 StorageOwnerGroupSpec group;
 if(!rt_layout(&group,rt_root,atomic_load_explicit(&rt_session,memory_order_acquire),next)){
  atomic_store_explicit(&rt_rot_stage,941u,memory_order_release);return false;
 }
 if(!rt_epoch_dir(next)){atomic_store_explicit(&rt_rot_stage,942u,memory_order_release);atomic_store_explicit(&rt_stage,930u,memory_order_release);return false;}
 memset(&rt_next,0,sizeof(rt_next));
 rt_next.replace_layout=true;
 rt_next.segment_seed=atomic_load_explicit(&rt_session,memory_order_acquire)^UINT64_C(0x5A5A5A5A)^(uint64_t)next;
 for(unsigned k=0;k<STORAGE_OWNER_FILES;k++)rt_next.files[k]=group.data[k];
 rt_next.ledger=group.ledger;rt_next.catalog=group.catalog;
 StorageResult r=storage_owner_begin_rotation(&rt_owner,&rt_next);
 if(r!=STORE_OK){atomic_store_explicit(&rt_rot_stage,943u,memory_order_release);atomic_store_explicit(&rt_rot_error,(unsigned)r,memory_order_release);
  atomic_store_explicit(&rt_error,(unsigned)r,memory_order_release);atomic_store_explicit(&rt_stage,931u,memory_order_release);return false;}
 atomic_store_explicit(&rt_rot_stage,944u,memory_order_release);
 atomic_store_explicit(&rt_stage,932u,memory_order_release);
 return true;
}
static bool rt_stop_ready(void){
 CaptureRouteFacts f;CaptureRouteHealth h;
 return capture_storage_route_facts_copy(&rt_owner.workspace.route,&f)&&
  capture_storage_route_health(&rt_owner.workspace.route,&h)&&h.attached&&h.producer_closed&&!h.consumer.blocked&&
  f.generation==h.generation&&f.final_clean&&f.final_capture_seq&&f.final_drain_seq&&
  f.identity.session_id==rt_last_facts.identity.session_id&&f.identity.capture_id==rt_last_facts.identity.capture_id&&
  f.identity.config.config_id==rt_last_facts.identity.config.config_id;
}
static bool rt_hold_for_stop(unsigned phase,StorageOwnerState state){
 if(!atomic_load_explicit(&rt_stop_intent,memory_order_acquire))return false;
 if(phase==3&&state==OWNER_RUNNING)
  atomic_store_explicit(&rt_stop_intent,2u,memory_order_release);
 return true;
}
static StorageResult rt_run_session(uint64_t session){
 atomic_store_explicit(&rt_session,session,memory_order_release);
 (void)snprintf(rt_root,sizeof(rt_root),"SR%06X",(unsigned)(session&0xFFFFFFu));
 rt_source_off=true;rt_stop=false;rt_capture_close=false;rt_stop_intent=0;atomic_store_explicit(&rt_ready,false,memory_order_release);
 atomic_store_explicit(&rt_active,true,memory_order_release);
 atomic_store_explicit(&rt_closed,false,memory_order_release);
 atomic_store_explicit(&rt_failed,false,memory_order_release);
 atomic_store_explicit(&rt_phase,0u,memory_order_release);
 atomic_store_explicit(&rt_rotations,0u,memory_order_release);
 atomic_store_explicit(&rt_epoch,0u,memory_order_release);
 atomic_store_explicit(&rt_generation,0u,memory_order_release);
 atomic_store_explicit(&rt_active_since,0u,memory_order_release);
 atomic_store_explicit(&rt_rotate_request,false,memory_order_release);
 if(rt_mutex_lock(NULL)){memset(&rt_diagnostics,0,sizeof(rt_diagnostics));rt_mutex_unlock(NULL);}
 if(!rt_make_dir()){rt_latch_failure();return STORE_INVALID;}
 StorageOwnerConfig oc;StorageOwnerFreertosConfig fc;rt_build_configs(&oc,&fc);
 if(!summary_runtime_init(&rt_summary,session,1)){rt_latch_failure();return STORE_INVALID;}
 EventRuntimeRollOps event_ops={NULL,rt_event_begin,rt_event_status};
 if(!event_feed_init(&rt_event_feed,session)||!event_runtime_init(&rt_events,session,&rt_history,&rt_summary,&event_ops)||!event_runtime_use_feed(&rt_events,&rt_event_feed)){rt_latch_failure();return STORE_INVALID;}
 marker_runtime_init(&rt_markers,session,&rt_summary);
 atomic_store_explicit(&rt_stage,5u,memory_order_release);
 if(!lfn_storage_owner_freertos_mount(&rt_fs,&rt_owner,&fc,&oc)){
  atomic_store_explicit(&rt_stage,6u+rt_fs.stage,memory_order_release);
  if(rt_fs.stage>=7)atomic_store_explicit(&rt_error,storage_owner_attach_trace,memory_order_release);
  rt_latch_failure();return STORE_INVALID;
 }
 atomic_store_explicit(&rt_stage,101u,memory_order_release);
 for(unsigned i=0;i<6000;i++){
  storage_owner_step(&rt_owner,now_us());
  if(storage_owner_state(&rt_owner)==OWNER_READY)break;
  if(storage_owner_state(&rt_owner)==OWNER_FAILED){rt_latch_failure();return STORE_INVALID;}
  vTaskDelay(1);
 }
 if(storage_owner_state(&rt_owner)!=OWNER_READY){rt_latch_failure();return STORE_INVALID;}
 atomic_store_explicit(&rt_stage,100u,memory_order_release);
 StorageSessionFacts sf;
 static const char *dev="LFN_AX7021";
 static const char hash[65]="0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
 memset(&sf,0,sizeof(sf));sf.device_id=dev;sf.session_id=session;
 sf.boot_generation=0;sf.start_ns=now_us()*1000;sf.build_hash=hash;sf.temporary=true;
 if(storage_owner_begin_cold(&rt_owner,&sf)!=STORE_OK){rt_latch_failure();return STORE_INVALID;}
 unsigned phase=1;bool setup_waiting=false;unsigned stop_wait=0,seal_batch=0;uint64_t setup_since=0;
 for(;;){
  atomic_fetch_add_explicit(&rt_steps,1u,memory_order_relaxed);
  capture_storage_route_step(&rt_owner.workspace.route);
  if(!storage_event_roll_active(&rt_owner.event_roll)){
   StorageOwnerGroup *g=&rt_owner.workspace.groups[rt_owner.group];
   uint64_t t=now_us();
   for(unsigned i=0;i<STORAGE_OWNER_FILES;i++)storage_pump_step(&g->pumps[i],t);
  }
  marker_runtime_step(&rt_markers,now_us());
  if(rt_markers.error)rt_restart_failure(972u,rt_markers.error);
  summary_runtime_step(&rt_summary);
  if(rt_summary.error)rt_restart_failure(970u,rt_summary.error);
  event_runtime_step(&rt_events,now_us());
  if(rt_events.error)rt_restart_failure(971u,rt_events.error);
  storage_owner_step(&rt_owner,now_us());
  StorageOwnerState st=storage_owner_state(&rt_owner);
  bool hold_for_stop=rt_hold_for_stop(phase,st);
  atomic_store_explicit(&rt_state,(unsigned)st,memory_order_release);
  atomic_store_explicit(&rt_error,(unsigned)storage_owner_error(&rt_owner),memory_order_release);
  if(st!=OWNER_FAILED&&atomic_load_explicit(&rt_steps,memory_order_relaxed)%100u==0u)(void)rt_publish_diagnostics();
  if(st==OWNER_WAIT_FACTS||st==OWNER_RUNNING)atomic_store_explicit(&rt_ready,true,memory_order_release);
  if(st==OWNER_FAILED){
   if(atomic_load_explicit(&rt_rot_stage,memory_order_acquire)>=940u)
    atomic_store_explicit(&rt_rot_error,((unsigned)storage_owner_error(&rt_owner)&0xffu)|((unsigned)rt_owner.failed_at<<8),memory_order_release);
   if(rt_owner.failed_at==OWNER_WAIT_FACTS){
    if(atomic_load_explicit(&rt_stage,memory_order_acquire)!=968u){
     unsigned bcode=rt_capture_code(&rt_last_facts,&rt_cfg);
     atomic_store_explicit(&rt_stage,700u+bcode,memory_order_release);
     atomic_store_explicit(&rt_error,((unsigned)rt_last_facts.active.bound_mask&0xffffu)|((unsigned)(rt_last_facts.started.flags&0xffu)<<16),memory_order_release);
    }
   }else if(rt_owner.failed_at==OWNER_COLD)atomic_store_explicit(&rt_stage,300u+rt_owner.cold.failed_at,memory_order_release);
   else atomic_store_explicit(&rt_stage,200u+(unsigned)rt_owner.failed_at,memory_order_release);
   atomic_store_explicit(&rt_error,(unsigned)storage_owner_error(&rt_owner),memory_order_release);
   break;
  }
  if(phase==1&&st==OWNER_WAIT_FACTS){
   if(atomic_load_explicit(&rt_stop,memory_order_acquire)&&++stop_wait>RT_STOP_WAIT_TICKS){rt_restart_failure(969u,STORE_EXPIRED);continue;}
   CaptureRouteFacts facts;memset(&facts,0,sizeof(facts));
   if(capture_storage_route_facts_copy(&rt_owner.workspace.route,&facts)){
    rt_last_facts=facts;
    unsigned flags=(facts.capture_valid?1u:0)|(facts.active_valid?2u:0)|(facts.started_valid?4u:0)|(facts.software_drained?8u:0)|(facts.version?16u:0);
    atomic_store_explicit(&rt_stage,400u+flags,memory_order_release);
   }
   uint64_t current=now_us();
   /* BEGIN may legitimately wait for a later user START. Bound pairing only
    * after capture has actually published a started identity. */
   if(!setup_waiting&&facts.started_valid&&facts.identity.capture_id){setup_waiting=true;setup_since=current;}
   if(setup_waiting&&(current<setup_since||current-setup_since>=UINT64_C(10000000))){
    rt_restart_failure(968u,STORE_EXPIRED);continue;
   }
   if(facts.version&&facts.capture_valid&&facts.active_valid&&facts.started_valid&&!facts.software_drained){
    rt_cfg=facts.identity.config;
    StorageResult sr=rt_begin_segment(&facts,true,NULL);
    if(sr==STORE_OK)phase=2;else atomic_store_explicit(&rt_error,(unsigned)sr,memory_order_release);
   }
  }else if(phase==2&&st==OWNER_RUNNING){
   marker_runtime_enable(&rt_markers,rt_last_facts.identity.capture_id,rt_cfg.config_id);
   atomic_store_explicit(&rt_active_since,now_us(),memory_order_release);phase=3;
  }else if(phase==3&&st==OWNER_RUNNING){
   if(atomic_load_explicit(&rt_stop,memory_order_acquire)){
    if(rt_stop_ready()){
     StorageResult r=storage_owner_request_stop(&rt_owner);
     if(r==STORE_OK)phase=4;else if(r!=STORE_BUSY)rt_restart_failure(969u,r);
    }else if(++stop_wait>RT_STOP_WAIT_TICKS)rt_restart_failure(969u,STORE_EXPIRED);
   }else if(!hold_for_stop){
    StorageOwnerGroup *g=&rt_owner.workspace.groups[rt_owner.group];
    uint64_t bytes=0;
    for(unsigned k=0;k<STORAGE_OWNER_FILES;k++){
     uint64_t file=g->writers[k].file_bytes;
     bytes=file>UINT64_MAX-bytes?UINT64_MAX:bytes+file;
    }
    uint64_t since=atomic_load_explicit(&rt_active_since,memory_order_acquire);
    bool due=atomic_load_explicit(&rt_rotate_request,memory_order_acquire)||
              (atomic_load_explicit(&rt_auto_rotate,memory_order_acquire)&&since&&now_us()-since>=atomic_load_explicit(&rt_max_age,memory_order_acquire))||
              (atomic_load_explicit(&rt_auto_rotate,memory_order_acquire)&&bytes>=atomic_load_explicit(&rt_max_bytes,memory_order_acquire));
    if(due&&!storage_event_roll_active(&rt_owner.event_roll)&&!rt_events.roll_pending){
     atomic_store_explicit(&rt_rotate_request,false,memory_order_release);
     if(rt_start_rotation())phase=5;
     else vTaskDelay(1);
    }
   }
  }else if(phase==5&&st==OWNER_RUNNING){
   atomic_fetch_add_explicit(&rt_epoch,1u,memory_order_release);
   atomic_store_explicit(&rt_generation,rt_owner.workspace.route.published.generation,memory_order_release);
   if(rt_cfg.config_id==UINT32_MAX||atomic_load_explicit(&rt_stop,memory_order_acquire))
    rt_restart_failure(960u,STORE_INVALID);
   else{
    rt_restart_cfg=rt_cfg;rt_restart_cfg.config_id=rt_cfg.config_id+1u;
    rt_restart_since=now_us();rt_segment_token=0;
    atomic_store_explicit(&rt_restart_pending,true,memory_order_release);
    atomic_store_explicit(&rt_stage,961u,memory_order_release);phase=6;
   }
  }else if(phase==6&&st==OWNER_RUNNING){
   CaptureRouteFacts facts={0};
   if(rt_restart_expired(now_us()))rt_restart_failure(962u,STORE_EXPIRED);
   else if(capture_storage_route_facts_copy(&rt_owner.workspace.route,&facts)&&
           facts.version>rt_last_facts.version&&facts.capture_valid&&facts.active_valid&&facts.started_valid&&!facts.software_drained&&
           facts.identity.session_id==atomic_load_explicit(&rt_session,memory_order_acquire)&&
           facts.identity.capture_id!=rt_last_facts.identity.capture_id&&
           facts.identity.config.config_id==rt_restart_cfg.config_id&&
           facts.identity.config.mode==rt_restart_cfg.mode&&facts.identity.config.source==rt_restart_cfg.source&&
           facts.identity.config.channel==rt_restart_cfg.channel&&facts.identity.config.pattern==rt_restart_cfg.pattern){
    StorageResult r=rt_begin_segment(&facts,false,&rt_segment_token);
    if(r==STORE_OK){rt_last_facts=facts;phase=7;}
    else if(r!=STORE_BUSY&&r!=STORE_CAPACITY&&r!=STORE_DEPENDENCY)rt_restart_failure(964u,r);
   }else if(atomic_load_explicit(&rt_stop,memory_order_acquire))rt_restart_failure(963u,STORE_INVALID);
  }else if(phase==7&&st==OWNER_RUNNING){
   StorageSegment *segment=&rt_owner.workspace.groups[rt_owner.group].segment;
   if(segment->state==SEGMENT_FAILED)rt_restart_failure(965u,segment->error);
   else if(segment->state==SEGMENT_SYNCED){
    StorageResult r=storage_segment_ack(segment,rt_segment_token);
    if(r!=STORE_OK)rt_restart_failure(966u,r);
    else{
     rt_cfg=rt_restart_cfg;marker_runtime_enable(&rt_markers,rt_last_facts.identity.capture_id,rt_cfg.config_id);atomic_fetch_add_explicit(&rt_rotations,1u,memory_order_release);
     atomic_store_explicit(&rt_active_since,now_us(),memory_order_release);
     atomic_store_explicit(&rt_stage,100u,memory_order_release);phase=3;
    }
   }else if(rt_restart_expired(now_us()))rt_restart_failure(967u,STORE_EXPIRED);
  }else if(phase==4&&st==OWNER_STOPPED)break;
  if(atomic_load_explicit(&rt_failed,memory_order_acquire))break;
  atomic_store_explicit(&rt_phase,phase,memory_order_release);
  rt_schedule_delay(&seal_batch);
 }
 return rt_finish_session();
}
static void rt_entry(void *v){
 (void)v;portTASK_USES_FLOATING_POINT();
 rt_task=xTaskGetCurrentTaskHandle();
 for(;;){
  RtMsg m;
  if(xQueueReceive(rt_queue,&m,portMAX_DELAY)!=pdTRUE)continue;
  if(m.kind==RT_CMD_SESSION&&!atomic_load_explicit(&rt_failed,memory_order_acquire))(void)rt_run_session(m.session);
  else if(m.kind==RT_CMD_STOP){atomic_store_explicit(&rt_stop,true,memory_order_release);atomic_store_explicit(&rt_capture_close,true,memory_order_release);}
 }
}
bool lfn_storage_runtime_start(void){
 if(rt_started)return true;
 history_init(&rt_history);
 rt_mutex=xSemaphoreCreateMutex();
 rt_queue=xQueueCreate(2,sizeof(RtMsg));
 if(!rt_mutex||!rt_queue)return false;
 if(xTaskCreate(rt_entry,"storage_rt",RT_STACK_WORDS,NULL,3,&rt_task)!=pdPASS)return false;
 atomic_store_explicit(&rt_started,true,memory_order_release);
 return true;
}
PcmHistory *lfn_storage_runtime_history(void){return &rt_history;}
bool lfn_storage_runtime_marker(const LfnMarker*m){
 return m&&atomic_load_explicit(&rt_active,memory_order_acquire)&&atomic_load_explicit(&rt_phase,memory_order_acquire)==3u&&
  !atomic_load_explicit(&rt_stop,memory_order_acquire)&&!atomic_load_explicit(&rt_failed,memory_order_acquire)&&marker_runtime_enqueue(&rt_markers,m);
}
bool lfn_storage_runtime_session(uint64_t host_session){
 if(!rt_started||!host_session||atomic_load_explicit(&rt_failed,memory_order_acquire))return false;
 if(atomic_load_explicit(&rt_active,memory_order_acquire))return false;
 RtMsg m={RT_CMD_SESSION,host_session};
 if(xQueueSend(rt_queue,&m,0)!=pdTRUE)return false;
 atomic_store_explicit(&rt_session,host_session,memory_order_release);
 return true;
}
bool lfn_storage_runtime_prepare_stop(void){
 if(!atomic_load_explicit(&rt_active,memory_order_acquire)||atomic_load_explicit(&rt_failed,memory_order_acquire))return true;
 unsigned idle=0;
 (void)atomic_compare_exchange_strong_explicit(&rt_stop_intent,&idle,1u,memory_order_acq_rel,memory_order_acquire);
 return atomic_load_explicit(&rt_stop_intent,memory_order_acquire)==2u;
}
void lfn_storage_runtime_stop(void){
 if(!rt_started)return;
 atomic_store_explicit(&rt_stop,true,memory_order_release);
 atomic_store_explicit(&rt_capture_close,true,memory_order_release);
 RtMsg m={RT_CMD_STOP,0};
 (void)xQueueSend(rt_queue,&m,0);
}
void lfn_storage_runtime_capture_stopped(bool stopped){atomic_store_explicit(&rt_capture_stopped,stopped,memory_order_release);}
bool lfn_storage_runtime_take_restart(PlCaptureConfig *config){
 if(!config||atomic_load_explicit(&rt_failed,memory_order_acquire)||!atomic_load_explicit(&rt_restart_pending,memory_order_acquire))return false;
 *config=rt_restart_cfg;atomic_store_explicit(&rt_restart_pending,false,memory_order_release);return true;
}
bool lfn_storage_runtime_limits(uint64_t max_file_bytes,uint64_t max_file_age_us){
 if(!max_file_bytes||!max_file_age_us)return false;
 atomic_store_explicit(&rt_max_bytes,max_file_bytes,memory_order_release);
 atomic_store_explicit(&rt_max_age,max_file_age_us,memory_order_release);return true;
}
bool lfn_storage_runtime_request_rotation(void){
 if(!rt_started||atomic_load_explicit(&rt_failed,memory_order_acquire))return false;
 atomic_store_explicit(&rt_rotate_request,true,memory_order_release);
 return true;
}
bool lfn_storage_runtime_auto_rotate(bool enable){atomic_store_explicit(&rt_auto_rotate,enable,memory_order_release);return true;}
bool lfn_storage_runtime_capture_close_requested(void){return atomic_load_explicit(&rt_capture_close,memory_order_acquire);}
bool lfn_storage_runtime_ready_for_start(void){return atomic_load_explicit(&rt_ready,memory_order_acquire);}
bool lfn_storage_runtime_failed(void){return atomic_load_explicit(&rt_failed,memory_order_acquire);}
bool lfn_storage_runtime_health(StorageRuntimeHealth *out){
 if(!out)return false;
 memset(out,0,sizeof(*out));
 out->started=atomic_load_explicit(&rt_started,memory_order_acquire);
 out->active=atomic_load_explicit(&rt_active,memory_order_acquire);
 out->closed=atomic_load_explicit(&rt_closed,memory_order_acquire);
 out->failed=atomic_load_explicit(&rt_failed,memory_order_acquire);
 out->session=atomic_load_explicit(&rt_session,memory_order_acquire);
 out->steps=atomic_load_explicit(&rt_steps,memory_order_acquire);
 out->state=atomic_load_explicit(&rt_state,memory_order_acquire);
 out->phase=atomic_load_explicit(&rt_phase,memory_order_acquire);
 out->error=atomic_load_explicit(&rt_error,memory_order_acquire);
 out->sd_stage=atomic_load_explicit(&rt_stage,memory_order_acquire);
 out->rotations=atomic_load_explicit(&rt_rotations,memory_order_acquire);
 out->epoch=atomic_load_explicit(&rt_epoch,memory_order_acquire);
 out->route_generation=atomic_load_explicit(&rt_generation,memory_order_acquire);
 out->restart_pending=atomic_load_explicit(&rt_restart_pending,memory_order_acquire);
 out->rotation_stage=atomic_load_explicit(&rt_rot_stage,memory_order_acquire);
 out->rotation_error=atomic_load_explicit(&rt_rot_error,memory_order_acquire);
 if(rt_mutex_lock(NULL)){out->diagnostics=rt_diagnostics;rt_mutex_unlock(NULL);}
 /* A stale periodic sample is not a successful final snapshot. Its
  * step remains visible, but valid=false explicitly marks it unavailable. */
 if((out->failed||out->closed)&&out->diagnostics.step!=out->steps)out->diagnostics.valid=false;
 return true;
}
