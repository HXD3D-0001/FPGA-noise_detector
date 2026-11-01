#include "storage_task_freertos.h"
#include "../app/capture_image.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "xiltimer.h"
#include "xtimer_config.h"
#include "../time/timer_scale.h"
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#if configUSE_TASK_FPU_SUPPORT != 1 || configMAX_PRIORITIES < 8 || configUSE_TRACE_FACILITY != 1
#error Require matching FPU/trace FreeRTOS BSP
#endif
_Static_assert(sizeof(StackType_t)==4,"Stack depth is words of32bits");
enum { SD_CLOSE_ATTEMPTS=100, SD_CLOSE_WAIT_MS=30000 };
StorageTaskResources lfn_storage_resources __attribute__((section(".lfn_data.storage"),aligned(64),used));
/* Read from actual ELF to avoid using Host sizeof for target budgets. Appended
 * FatFs/FIL/volume entries make the in-task mount/open memory explicit. */
const uint32_t lfn_storage_resource_sizes[] __attribute__((used))={sizeof(StorageTaskResources),sizeof(StorageMetadataRegistry),sizeof(StorageWriter),sizeof(StorageQueue),sizeof(StorageService),sizeof(StorageMetadataBuilder),sizeof(StoragePump),sizeof(StackType_t),configTOTAL_HEAP_SIZE,STORAGE_TASK_STACK_WORDS,configMINIMAL_STACK_SIZE,configTIMER_TASK_STACK_DEPTH,sizeof(CaptureStorageRoute),sizeof(FATFS),sizeof(FIL),sizeof(LfnFatFsVolume)
#ifdef LFN_STORAGE_OWNER_INSTANCE
 ,sizeof(StorageOwner),sizeof(StorageOwnerFreertos)
#endif
};
static StorageBackend io;static SemaphoreHandle_t mailbox,route_mutex;static TaskHandle_t owner;
static atomic_bool started,ready,failed,connection_requested,sd_close_requested;
static atomic_bool sd_mode,sd_mounted,sd_closed,sd_close_failed,sd_existing,sd_tail_repaired,sd_recovery_only;
static _Atomic int sd_fault;
static _Atomic unsigned sd_stage,sd_last_error,loop_steps,loop_stage;
static _Atomic uint64_t sd_recovered,sd_prefix,sd_file_bytes;
static _Atomic int sd_tail_reason;
static uint64_t session_id,file_id,sequence_seed,sync_period;
static TickType_t sd_close_since;static unsigned sd_close_tries;
static char sd_relative_path[LFN_FATFS_PATH];
static bool fs_owner(void*v){(void)v;return owner&&xTaskGetCurrentTaskHandle()==owner;}
static bool mlock(void*v){(void)v;return xSemaphoreTake(mailbox,0)==pdTRUE;}
static void munlock(void*v){(void)v;(void)xSemaphoreGive(mailbox);}
static bool route_lock(void*v){(void)v;return xSemaphoreTake(route_mutex,0)==pdTRUE;}
static void route_unlock(void*v){(void)v;(void)xSemaphoreGive(route_mutex);}
static StorageResult sink_energy(void*v,const LfnServiceMessage*m,StoragePumpTicket*t){return storage_pump_energy(v,m,t);}
static StorageResult sink_psd(void*v,const LfnSpectrum*s,StoragePumpTicket*t){return storage_pump_psd(v,s,t);}
static StorageResult sink_pcm(void*v,uint64_t session,const uint8_t*b,size_t n,StoragePumpTicket*t){return storage_pump_pcm(v,session,b,n,t);}
static StorageResult sink_loss(void*v,const StorageIngressLoss*m,StoragePumpTicket*t){return storage_pump_ingress_loss(v,m,t);}
static StorageResult sink_poll(void*v,StoragePumpTicket t,StoragePumpResult*out){return storage_pump_poll(v,t,out);}
static StorageResult sink_ack(void*v,StoragePumpTicket t){return storage_pump_ack(v,t);}
static StorageResult sink_close(void*v){StorageResult r=storage_pump_close(v);if(r==STORE_OK)atomic_store_explicit(&sd_close_requested,true,memory_order_release);return r;}
static StorageIoResult size_io(void*v,uint64_t*n){(void)v;return io.size(io.context,n);}
static StorageIoResult read_io(void*v,uint64_t o,uint8_t*b,size_t n,size_t*a){(void)v;return io.read(io.context,o,b,n,a);}
static StorageIoResult write_io(void*v,uint64_t o,const uint8_t*b,size_t n,size_t*a){(void)v;return io.write(io.context,o,b,n,a);}
static StorageIoResult sync_io(void*v){(void)v;return io.sync(io.context);}
static StorageIoResult truncate_io(void*v,uint64_t n){(void)v;return io.truncate(io.context,n);}
static uint64_t now_us(void){XTime n;XTime_GetTime(&n);return lfn_timer_ticks_us(n,COUNTS_PER_SECOND);}
static void sd_fault_set(StorageIoResult r){atomic_store_explicit(&sd_fault,(int)r,memory_order_release);}
static void sd_stage_set(unsigned s){atomic_store_explicit(&sd_stage,s,memory_order_release);}
static void sd_mounted_set(bool v){atomic_store_explicit(&sd_mounted,v,memory_order_release);}
#ifdef LFN_STORAGE_OWNER_INSTANCE
static atomic_bool mf_mode,mf_ready,mf_failed,mf_done,mf_write_mode;
static _Atomic unsigned mf_owner_state,mf_owner_error,mf_phase;
static _Atomic uint64_t mf_steps,mf_written;
static _Atomic unsigned mf_write_target;
static LfnSpectrum mf_write_spectrum;
static char mf_root[16];
static uint64_t mf_session,mf_seed,mf_sync;
static StorageOwnerNamespaceName mf_names[12];
static LfnFatFsNamespaceName mf_fs_names[12];
static StorageOwnerFileSpec mf_specs[2][4];
static StorageOwnerGroupSpec mf_groups[2];
typedef struct{bool pending;uint64_t ticket,serial;CaptureControlReceipt reply;} MfMail;
static MfMail mf_cold_mail,mf_rot_mail;
static LfnServiceBridge mf_bridge;static LfnServiceLocks mf_bridge_locks;
static PlCaptureConfig mf_cfg_config;
static StorageSessionFacts mf_session_facts;
static StorageConfigFacts mf_config_facts;
static StorageCaptureFacts mf_capture_facts;
static CaptureRouteFacts mf_route_facts;
static StorageOwner *mf_owner;
static void mf_route_generation(uint64_t gen);
static bool mf_proof(void *c){(void)c;return true;}
static StorageIoResult mf_media(void *c){(void)c;return STORE_IO_OK;}
static StorageResult mf_cold_attach(void *ctx,CaptureStorageRoute *r,uint64_t *t){
 MfMail*m=ctx;if(!m||!r||!t||m->pending)return STORE_BUSY;
 if(!capture_storage_route_bind(r,&mf_bridge))return STORE_INVALID;
 r->published.generation=1;
 m->pending=true;m->ticket=++m->serial;*t=m->ticket;
 m->reply=(CaptureControlReceipt){m->ticket,CONTROL_ATTACH,STORE_OK,{false,0,1}};
 return STORE_OK;
}
static StorageResult mf_cold_copy(void *c,uint64_t t,CaptureControlReceipt *o){MfMail*m=c;if(!m||!o||!m->pending||m->ticket!=t)return STORE_BUSY;*o=m->reply;return STORE_OK;}
static StorageResult mf_cold_ack(void *c,uint64_t t){MfMail*m=c;if(!m||!m->pending||m->ticket!=t)return STORE_BUSY;m->pending=false;return STORE_OK;}
static StorageResult mf_rot_submit(void *c,CaptureControlKind k,uint64_t pause,uint64_t gen,uint64_t *t){
 MfMail*m=c;(void)pause;if(!m||!t||m->pending)return STORE_BUSY;
 m->ticket=++m->serial;*t=m->ticket;m->pending=true;
 uint64_t rg=mf_owner?mf_owner->workspace.route.published.generation:1;
 if(k==CONTROL_PAUSE)m->reply=(CaptureControlReceipt){m->ticket,CONTROL_PAUSE,STORE_OK,{true,m->ticket,rg}};
 else if(k==CONTROL_STOP_CLOSE)m->reply=(CaptureControlReceipt){m->ticket,CONTROL_STOP_CLOSE,STORE_OK,{false,0,rg}};
 else{mf_route_generation(gen);m->reply=(CaptureControlReceipt){m->ticket,CONTROL_RESUME,STORE_OK,{false,0,gen}};}
 return STORE_OK;
}
static StorageResult mf_rot_copy(void *c,uint64_t t,CaptureControlReceipt *o){
 MfMail*m=c;if(!m||!o||!m->pending||m->ticket!=t)return STORE_BUSY;
 *o=m->reply;
 if(o->kind==CONTROL_STOP_CLOSE&&o->result==STORE_OK&&mf_owner)
  (void)capture_storage_route_producer_close(&mf_owner->workspace.route);
 return STORE_OK;
}
static StorageResult mf_rot_ack(void *c,uint64_t t){MfMail*m=c;if(!m||!m->pending||m->ticket!=t)return STORE_BUSY;m->pending=false;return STORE_OK;}
static void mf_route_generation(uint64_t gen){if(mf_owner)mf_owner->workspace.route.published.generation=gen;}
static bool mf_configs(StorageOwnerConfig *oc,StorageOwnerFreertosConfig *fc,LfnFatFsVolume *v){
 static const char *names[12]={"AMON.LFR","ARAW.LFR","AIDX.SFL","ADIR.SFC",
                               "BMON.LFR","BRAW.LFR","BIDX.SFL","BDIR.SFC",
                               "CMON.LFR","CRAW.LFR","CIDX.SFL","CDIR.SFC"};
 static const StorageCatalogRole roles[4]={CATALOG_MONITOR,CATALOG_RAW,CATALOG_MONITOR,CATALOG_MONITOR};
 memset(oc,0,sizeof(*oc));memset(fc,0,sizeof(*fc));memset(mf_specs,0,sizeof(mf_specs));
 for(unsigned k=0;k<12;k++){
  strncpy(mf_names[k].name,names[k],sizeof(mf_names[k].name)-1);
  mf_names[k].snapshot_metadata=(k%4)==3;
  strncpy(mf_fs_names[k].name,names[k],sizeof(mf_fs_names[k].name)-1);
  mf_fs_names[k].snapshot_metadata=(k%4)==3;
 }
 for(unsigned g=0;g<2;g++){
  for(unsigned k=0;k<4;k++){
   StorageOwnerFileSpec *s=&mf_specs[g][k];
   s->file_id=(mf_session^((uint64_t)(0x1000u*g+k+1)<<32)^UINT64_C(0xA5A50000))|((uint64_t)(g*4+k+1));
   s->order_key=(uint64_t)(g*4+k+1);s->role=roles[k];s->family=RETAIN_PSD;
   (void)snprintf(s->path,sizeof(s->path),"%s/%s",mf_root,names[g*4+k]);
   (void)snprintf(s->name,sizeof(s->name),"%s",names[g*4+k]);
  }
  uint64_t ledger_id=(mf_session^UINT64_C(0x100000000000))|((uint64_t)(g+1)<<8)|1;
  uint64_t catalog_id=(mf_session^UINT64_C(0x200000000000))|((uint64_t)(g+1)<<8)|2;
  for(unsigned k=0;k<2;k++){mf_specs[g][k].ledger_id=ledger_id;mf_specs[g][k].catalog_id=catalog_id;}
  mf_specs[g][2].file_id=ledger_id;mf_specs[g][3].file_id=catalog_id;
  for(unsigned k=0;k<2;k++)if(mf_specs[g][k].file_id==ledger_id||mf_specs[g][k].file_id==catalog_id)
   mf_specs[g][k].file_id^=UINT64_C(0x400000000000);
  mf_groups[g].data[0]=mf_specs[g][0];mf_groups[g].data[1]=mf_specs[g][1];
  mf_groups[g].ledger=mf_specs[g][2];mf_groups[g].catalog=mf_specs[g][3];
  mf_groups[g].config_id=0;mf_groups[g].capture_id=0;
 }
 StorageOwnerPorts ports;memset(&ports,0,sizeof(ports));
 ports.cold=(StorageColdCapturePort){&mf_cold_mail,mf_cold_attach,mf_cold_copy,mf_cold_ack};
 ports.rotation=(StorageRotationCapturePort){&mf_rot_mail,mf_rot_submit,mf_rot_copy,mf_rot_ack};
 ports.is_owner=fs_owner;ports.owner_context=NULL;
 ports.producer_stopped=mf_proof;ports.proof_context=NULL;
 oc->session_id=mf_session;oc->next_sequence=mf_seed;oc->segment_seed=mf_seed^UINT64_C(0x5A5A);
 oc->multi_seed=mf_seed^UINT64_C(0xA5A5);oc->sync_us=mf_sync;
 oc->max_file_bytes=UINT64_C(1)<<20;oc->max_file_age_us=UINT64_C(1)<<40;
 oc->created_us=1;oc->owner_us=100000;oc->root=mf_root;
 oc->names=mf_names;oc->name_count=12;oc->targets[0]=0;oc->targets[1]=1;oc->targets[2]=0;oc->targets[3]=1;
 oc->groups[0]=mf_groups[0];oc->groups[1]=mf_groups[1];oc->ports=ports;
 oc->locks=(StorageQueueLocks){NULL,mlock,munlock};
 oc->route_locks=(StorageQueueLocks){NULL,route_lock,route_unlock};
 *fc=(StorageOwnerFreertosConfig){v,"0:",mf_root,mf_fs_names,12,mf_media,NULL,
   fs_owner,NULL,mf_proof,NULL,ports.cold,ports.rotation};
 return true;
}
static void mf_facts(void){
 mf_cfg_config=(PlCaptureConfig){0,PL_CAPTURE_TEST,0,3,1};
 static const char *dev="LFN_AX7021";
 static const char hash64[]="0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
 memset(&mf_session_facts,0,sizeof(mf_session_facts));
 mf_session_facts.device_id=dev;mf_session_facts.session_id=mf_session;
 mf_session_facts.boot_generation=0;mf_session_facts.start_ns=now_us()*1000;
 mf_session_facts.build_hash=hash64;mf_session_facts.temporary=true;
 memset(&mf_config_facts,0,sizeof(mf_config_facts));
 mf_config_facts.device_id=dev;mf_config_facts.config_hash=hash64;
 mf_config_facts.hardware_id="AX7021_PRE_ADC";mf_config_facts.gain_setting_id="UNVERIFIED";
 mf_config_facts.adc_id="NONE";mf_config_facts.microphone_id="NONE";mf_config_facts.point_pose_id="NONE";
 mf_config_facts.band_definition_id="LFN_THIRD_OCTAVE";mf_config_facts.session_id=mf_session;
 mf_config_facts.calibration_id=0;mf_config_facts.config=mf_cfg_config;
 mf_config_facts.event=(StorageEventFacts){6,3,60,5,20,5e-10,3,16};
 memset(&mf_capture_facts,0,sizeof(mf_capture_facts));
 mf_capture_facts.device_id=dev;mf_capture_facts.config=mf_cfg_config;
 mf_capture_facts.max_age_ticks=100;mf_capture_facts.revision=1;
 mf_capture_facts.start_ns=now_us()*1000;mf_capture_facts.mono_ns=mf_capture_facts.start_ns;
 mf_capture_facts.ns_per_raw=1e9/48000.0;mf_capture_facts.uncertainty_ns=10000000;
 mf_capture_facts.started.seq=2;
 mf_capture_facts.started.flags=PL_CAPTURE_F_RUNNING|PL_CAPTURE_F_POSITION_VALID|PL_CAPTURE_F_TEST;
 mf_capture_facts.started.capture_id=1;mf_capture_facts.started.active_config_id=1;
 mf_capture_facts.started.configured=mf_cfg_config;
 mf_capture_facts.active.seq=2;mf_capture_facts.active.kind=PL_CONTRACT_ACTIVE;
 mf_capture_facts.active.mode=mf_cfg_config.mode;mf_capture_facts.active.source=mf_cfg_config.source;
 mf_capture_facts.active.channel=mf_cfg_config.channel;mf_capture_facts.active.pattern=mf_cfg_config.pattern;
 mf_capture_facts.active.bound_mask=11;
 mf_capture_facts.active.plan=(CapturePlan){mf_session,1,1,false,{5,6,7,8}};
 mf_capture_facts.first_valid_raw=1679;mf_capture_facts.mono_raw=mf_capture_facts.first_valid_raw;
 for(unsigned k=0;k<4;k++)if(mf_capture_facts.active.bound_mask&(1u<<k))
  mf_capture_facts.active.anchors[k]=(CaptureAnchor){mf_session,1,1,k+1,
   mf_capture_facts.active.plan.first_seq[k],mf_capture_facts.first_valid_raw};
 memset(&mf_route_facts,0,sizeof(mf_route_facts));
 mf_route_facts.version=1;mf_route_facts.observed_us=now_us();
 mf_route_facts.identity=(LfnServiceIdentity){mf_session,1,mf_cfg_config};
 mf_route_facts.bound_mask=11;
 for(unsigned k=0;k<4;k++){mf_route_facts.first_seq[k]=mf_capture_facts.active.plan.first_seq[k];mf_route_facts.first_raw[k]=1679;}
 mf_route_facts.capture_valid=mf_route_facts.active_valid=mf_route_facts.started_valid=true;
 mf_route_facts.software_drained=false;
 mf_route_facts.started=mf_capture_facts.started;mf_route_facts.active=mf_capture_facts.active;
}
static void mf_next_layout(StorageOwnerNextSpec *next){
 static const char *names[4]={"CMON.LFR","CRAW.LFR","CIDX.SFL","CDIR.SFC"};
 static const StorageCatalogRole roles[4]={CATALOG_MONITOR,CATALOG_RAW,CATALOG_MONITOR,CATALOG_MONITOR};
 uint64_t ledger=(mf_session^UINT64_C(0x300000000000))|0x101;
 uint64_t catalog=(mf_session^UINT64_C(0x400000000000))|0x102;
 memset(next,0,sizeof(*next));next->replace_layout=true;next->segment_seed=mf_seed+2;
 for(unsigned k=0;k<4;k++){
  StorageOwnerFileSpec *s=k<2?&next->files[k]:(k==2?&next->ledger:&next->catalog);
  s->file_id=(mf_session^((uint64_t)(0x3000u+k+1)<<32)^UINT64_C(0x5A5A0000))|((uint64_t)(0xC0u+k+1));
  if(k<2){s->catalog_id=catalog;s->ledger_id=ledger;s->order_key=0xC0u+k+1;}
  else if(k==2)s->order_key=0xC2;else s->order_key=0xC3;
  s->role=roles[k];s->family=RETAIN_PSD;
  (void)snprintf(s->path,sizeof(s->path),"%s/%s",mf_root,names[k]);
  (void)snprintf(s->name,sizeof(s->name),"%s",names[k]);
 }
 next->ledger.file_id=ledger;next->catalog.file_id=catalog;
}
static void mf_entry(StorageTaskResources *r){
 StorageOwnerConfig oc;StorageOwnerFreertosConfig fc;
 if(!mf_configs(&oc,&fc,&r->volume)){sd_stage_set(900);atomic_store_explicit(&mf_failed,true,memory_order_release);for(;;)vTaskDelay(1);}
 mf_owner=&r->owner;
 if(!lfn_storage_owner_freertos_mount(&r->owner_fs,&r->owner,&fc,&oc)){
  sd_stage_set(900+r->owner_fs.stage);atomic_store_explicit(&mf_failed,true,memory_order_release);for(;;)vTaskDelay(1);
 }
 mf_facts();mf_bridge_locks=(LfnServiceLocks){0};
 (void)lfn_service_bridge_init(&mf_bridge,&mf_bridge_locks);
 atomic_store_explicit(&mf_ready,true,memory_order_release);
 unsigned phase=0;bool stop_requested=false;uint64_t energy_until=0;
 for(;;){
  atomic_fetch_add_explicit(&mf_steps,1u,memory_order_relaxed);
  storage_owner_step(&r->owner,now_us());
  StorageOwnerState st=storage_owner_state(&r->owner);
  atomic_store_explicit(&mf_owner_state,(unsigned)st,memory_order_release);
  atomic_store_explicit(&mf_owner_error,(unsigned)storage_owner_error(&r->owner),memory_order_release);
  if(st==OWNER_FAILED){
   StorageOwnerGroup *g0=&r->owner.workspace.groups[0];
   sd_stage_set(800u+(unsigned)r->owner.failed_at);
   if(r->owner.failed_at==OWNER_WAIT_FACTS){
    sd_stage_set(810u+(unsigned)r->owner.cold.state);
    atomic_store_explicit(&sd_last_error,((unsigned)r->owner.cold.failed_at<<8)|(unsigned)r->owner.cold.error,memory_order_release);
   }
   atomic_store_explicit(&sd_last_error,(unsigned)g0->provision.error,memory_order_release);
   atomic_store_explicit(&mf_failed,true,memory_order_release);atomic_store_explicit(&mf_phase,phase,memory_order_release);
   for(;;)vTaskDelay(1);
  }
  if(phase==0&&st==OWNER_READY){
   if(storage_owner_begin_cold(&r->owner,&mf_session_facts)==STORE_OK)phase=1;
  }else if(phase==1&&st==OWNER_WAIT_FACTS){
   capture_storage_route_facts_publish(&r->owner.workspace.route,&mf_route_facts);
   if(storage_owner_segment(&r->owner,&mf_config_facts,&mf_capture_facts)==STORE_OK)phase=2;
  }else if(phase==2&&st==OWNER_RUNNING&&atomic_load_explicit(&mf_write_mode,memory_order_acquire)){
   phase=10;
  }else if(phase==2&&st==OWNER_RUNNING){
   LfnServiceIdentity id={mf_session,1,mf_cfg_config};
   CaptureEnergyMessage msg;memset(&msg,0,sizeof(msg));
   msg.record=(PlEnergy){8,1,1679,1000,1,LFN_VALID};
   (void)lfn_service_energy_offer(&mf_bridge,&id,&msg);
   energy_until=mf_steps+200;phase=3;
  }else if(phase==10&&st==OWNER_RUNNING){
   static uint64_t wseq;
   LfnServiceIdentity id={mf_session,1,mf_cfg_config};
   LfnSpectrum *spp=&mf_write_spectrum;memset(spp,0,sizeof(*spp));
   spp->session_id=mf_session;spp->capture_id=1;spp->config_id=1;
   spp->stream_seq=5+wseq;spp->first_raw=1679+wseq*98304u;spp->end_raw=spp->first_raw+196608;
   spp->flags=LFN_VALID;spp->valid=true;spp->test_source=true;spp->psd[10]=0.25f;
   if(capture_storage_route_psd(&r->owner.workspace.route,&id,spp))wseq++;
   atomic_store_explicit(&mf_written,wseq,memory_order_release);
  }else if(phase==3&&st==OWNER_RUNNING&&mf_steps>energy_until){
   StorageOwnerNextSpec next;memset(&next,0,sizeof(next));next.segment_seed=mf_seed+1;
   StorageResult br=storage_owner_begin_rotation(&r->owner,&next);
   if(br==STORE_OK)phase=4;else atomic_store_explicit(&sd_last_error,(unsigned)br,memory_order_release);
  }else if(phase==4&&st==OWNER_RUNNING){
   StorageOwnerNextSpec next;mf_next_layout(&next);
   StorageResult br2=storage_owner_begin_rotation(&r->owner,&next);
   if(br2==STORE_OK)phase=5;else atomic_store_explicit(&sd_last_error,(unsigned)br2|(storage_owner_next_trace<<8),memory_order_release);
  }else if(phase==5&&st==OWNER_RUNNING){
   phase=6;
  }else if(phase==6&&!stop_requested&&st==OWNER_RUNNING){
   (void)capture_storage_route_producer_close(&r->owner.workspace.route);
   if(storage_owner_request_stop(&r->owner)==STORE_OK)stop_requested=true;
  }else if(phase==6&&stop_requested&&st==OWNER_STOPPED){
   (void)lfn_storage_owner_freertos_unmount(&r->owner_fs);
   atomic_store_explicit(&mf_done,true,memory_order_release);phase=8;
  }
  atomic_store_explicit(&mf_phase,phase,memory_order_release);
  vTaskDelay(1);
 }
}
#endif
/* Open the FatFs mount and NEW 8.3 file. Runs only in the storage task; every
 * FatFs entry checks this same task through fs_owner(). */
static bool sd_open(StorageTaskResources*r){
 sd_stage_set(10);
 if(!lfn_fatfs_volume_init(&r->volume,"0:",fs_owner,NULL)){sd_fault_set(STORE_IO_ERROR);return false;}
 sd_stage_set(21);
 StorageIoResult m=lfn_fatfs_mount(&r->volume);if(m!=STORE_IO_OK){sd_fault_set(m);return false;}
 sd_stage_set(31);
 sd_mounted_set(true);
 bool create_new=!atomic_load_explicit(&sd_existing,memory_order_acquire);
 StorageIoResult o=lfn_fatfs_open(&r->file,&r->volume,r->relative_path,create_new,true);
 if(o!=STORE_IO_OK){atomic_store_explicit(&sd_last_error,lfn_fatfs_last_error(),memory_order_release);sd_stage_set(100+lfn_fatfs_last_open_stage());sd_fault_set(o);return false;}
 sd_stage_set(41);
 if(!lfn_fatfs_backend(&r->file,&io)){sd_fault_set(STORE_IO_ERROR);return false;}
 sd_stage_set(50);
 return true;
}
/* Best-effort startup cleanup. mounted stays true unless unmount is proved. */
static void sd_abort(StorageTaskResources*r){
 if(r->file.opened)(void)lfn_fatfs_close(&r->file);
 if(atomic_load_explicit(&sd_mounted,memory_order_acquire)&&lfn_fatfs_unmount(&r->volume)==STORE_IO_OK)
  sd_mounted_set(false);
}
/* The pump owner is the only closer. Once pump admission is closed (whoever
 * requested it) finish durability in this task: f_sync+f_close inside
 * lfn_fatfs_close, then unmount. A degraded gate is bounded, never infinite. */
static void sd_finish_close(StorageTaskResources*r){
 if(!atomic_load_explicit(&sd_mode,memory_order_acquire)||
    atomic_load_explicit(&sd_closed,memory_order_acquire)||
    atomic_load_explicit(&sd_close_failed,memory_order_acquire))return;
 if(!atomic_load_explicit(&sd_close_requested,memory_order_acquire))return;
 TickType_t now=xTaskGetTickCount();
 if(sd_close_since==0)sd_close_since=now;
 StorageServiceHealth*h=&r->service.health;
 bool drained=h->sealed&&h->drained&&!h->fatal&&!h->pending;
 if(!drained){
  if(now-sd_close_since<pdMS_TO_TICKS(SD_CLOSE_WAIT_MS))return;
  /* Data may be lost or the service wedged; still close the handle but keep
   * the failure visible. Closed never claims the data was persisted. */
  sd_fault_set(STORE_IO_ERROR);
 }
 if(r->file.opened&&lfn_fatfs_close(&r->file)!=STORE_IO_OK){sd_fault_set(STORE_IO_ERROR);if(++sd_close_tries>=SD_CLOSE_ATTEMPTS)atomic_store_explicit(&sd_close_failed,true,memory_order_release);return;}
 if(atomic_load_explicit(&sd_mounted,memory_order_acquire)&&lfn_fatfs_unmount(&r->volume)!=STORE_IO_OK){sd_fault_set(STORE_IO_ERROR);if(++sd_close_tries>=SD_CLOSE_ATTEMPTS)atomic_store_explicit(&sd_close_failed,true,memory_order_release);return;}
 sd_mounted_set(false);
 atomic_store_explicit(&sd_closed,true,memory_order_release);
}
static void entry(void*v){
 (void)v;portTASK_USES_FLOATING_POINT();owner=xTaskGetCurrentTaskHandle();StorageTaskResources*r=&lfn_storage_resources;
 memset(r,0,sizeof(*r));storage_registry_init(&r->registry);
#ifdef LFN_STORAGE_OWNER_INSTANCE
 if(atomic_load_explicit(&mf_mode,memory_order_acquire)){mf_entry(r);}
#endif
 bool mounted=false;
 if(atomic_load_explicit(&sd_mode,memory_order_acquire)){
  memcpy(r->relative_path,sd_relative_path,strlen(sd_relative_path)+1);
  mounted=sd_open(r);
 }
 StorageBackend backend={&r->registry,size_io,read_io,write_io,sync_io,truncate_io,storage_registry_validate};
 bool ok=mounted||!atomic_load_explicit(&sd_mode,memory_order_acquire);
 if(ok)ok=storage_writer_init(&r->writer,&backend,session_id,file_id,sequence_seed,sync_period);
 unsigned recover_tries=0;
 while(ok&&r->writer.state!=SW_READY){
  storage_writer_step(&r->writer,now_us());
  if(r->writer.state==SW_FAULT){
   /* Scan faults are recoverable by an explicit rescan; never patch around
    * a fault that persists (bounded tries only). */
   if(atomic_load_explicit(&sd_existing,memory_order_acquire)&&recover_tries<2&&storage_writer_recover(&r->writer)){recover_tries++;continue;}
   ok=false;
  }else if(r->writer.state==SW_TAIL){
   if(atomic_load_explicit(&sd_existing,memory_order_acquire)){
    atomic_store_explicit(&sd_tail_reason,(int)r->writer.tail_reason,memory_order_release);
    if(storage_writer_repair_tail(&r->writer)&&++recover_tries<=3)continue;
    ok=false;
   }else ok=false;
  }
  vTaskDelay(1);
 }
 if(ok){
  atomic_store_explicit(&sd_recovered,r->writer.recovered_records,memory_order_release);
  atomic_store_explicit(&sd_prefix,r->writer.prefix,memory_order_release);
  atomic_store_explicit(&sd_file_bytes,r->writer.file_bytes,memory_order_release);
  if(r->writer.state==SW_READY&&atomic_load_explicit(&sd_tail_reason,memory_order_acquire)!=0)atomic_store_explicit(&sd_tail_repaired,true,memory_order_release);
 }
 if(atomic_load_explicit(&sd_recovery_only,memory_order_acquire)){
  /* Recovery inspection only: no queue/service/pump is created over a file
   * whose receipts are historical. Close still performs f_sync+f_close+unmount. */
  atomic_store_explicit(&ready,true,memory_order_release);
  for(;;){
   if(atomic_load_explicit(&sd_close_requested,memory_order_acquire)&&!atomic_load_explicit(&sd_closed,memory_order_acquire)){
    (void)lfn_fatfs_close(&r->file);
    if(atomic_load_explicit(&sd_mounted,memory_order_acquire)&&lfn_fatfs_unmount(&r->volume)==STORE_IO_OK)sd_mounted_set(false);
    atomic_store_explicit(&sd_closed,true,memory_order_release);
   }
   vTaskDelay(1);
  }
 }
 StorageQueueLocks serialized={0},locks={NULL,mlock,munlock};
 ok=ok&&storage_queue_init(&r->queue,&r->writer,r->arena,sizeof(r->arena),&serialized)&&storage_service_init(&r->service,&r->queue,&r->registry)&&storage_pump_init(&r->pump,&r->service,&locks);
 StorageQueueLocks route_locks={NULL,route_lock,route_unlock};CaptureStorageSink sink={&r->pump,sink_energy,sink_psd,sink_pcm,sink_loss,sink_poll,sink_ack,sink_close};
 ok=ok&&capture_storage_route_init(&r->route,session_id,&route_locks,&sink);
 if(!ok){if(atomic_load_explicit(&sd_mode,memory_order_acquire))sd_abort(r);atomic_store_explicit(&failed,true,memory_order_release);for(;;)vTaskDelay(1);}
 atomic_store_explicit(&ready,true,memory_order_release);
 for(;;){
  atomic_fetch_add_explicit(&loop_steps,1u,memory_order_relaxed);
  atomic_store_explicit(&loop_stage,1u,memory_order_relaxed);capture_storage_route_step(&r->route);
  atomic_store_explicit(&loop_stage,2u,memory_order_relaxed);storage_pump_step(&r->pump,now_us());
  atomic_store_explicit(&loop_stage,3u,memory_order_relaxed);sd_finish_close(r);
  atomic_store_explicit(&loop_stage,4u,memory_order_relaxed);vTaskDelay(1);
 }
}
static bool claim_start(void){bool expected=false;return atomic_compare_exchange_strong(&started,&expected,true);}
static bool spawn_task(unsigned priority){
 if(xPortGetFreeHeapSize()<STORAGE_TASK_STACK_WORDS*sizeof(StackType_t)+STORAGE_TASK_HEAP_MARGIN||(mailbox=xSemaphoreCreateMutex())==NULL){atomic_store(&failed,true);return false;}
 route_mutex=xSemaphoreCreateMutex();if(!route_mutex){vSemaphoreDelete(mailbox);mailbox=NULL;atomic_store(&failed,true);return false;}
 TaskHandle_t created;
 if(xTaskCreate(entry,"storage",STORAGE_TASK_STACK_WORDS,NULL,(UBaseType_t)priority,&created)!=pdPASS){vSemaphoreDelete(mailbox);vSemaphoreDelete(route_mutex);mailbox=NULL;route_mutex=NULL;atomic_store(&failed,true);return false;}
 return true;
}
bool lfn_storage_task_start(const StorageBackend*b,uint64_t session,uint64_t file,uint64_t seed,uint64_t sync,unsigned priority){
 if(!b||!b->size||!b->read||!b->write||!b->sync||!b->truncate||!session||!file||!seed||seed==UINT64_MAX||!sync||priority<1||priority>=6||!xTaskGetTickCount())return false;
 if(!claim_start())return false;
 atomic_store(&sd_mode,false);io=*b;session_id=session;file_id=file;sequence_seed=seed;sync_period=sync;return spawn_task(priority);
}
bool lfn_storage_task_start_sd_existing(uint64_t session,uint64_t file,uint64_t seed,uint64_t sync,unsigned priority,const char *relative_path){
 if(!relative_path||!*relative_path||strlen(relative_path)>=LFN_FATFS_PATH||!session||!file||!seed||seed==UINT64_MAX||!sync||priority<1||priority>=6||!xTaskGetTickCount())return false;
 if(!claim_start())return false;
 atomic_store(&sd_mode,true);atomic_store(&sd_existing,true);atomic_store(&sd_recovery_only,true);
 memset(sd_relative_path,0,sizeof(sd_relative_path));memcpy(sd_relative_path,relative_path,strlen(relative_path)+1);
 session_id=session;file_id=file;sequence_seed=seed;sync_period=sync;return spawn_task(priority);
}
bool lfn_storage_task_start_sd(uint64_t session,uint64_t file,uint64_t seed,uint64_t sync,unsigned priority,const char *relative_path){
 if(!relative_path||!*relative_path||strlen(relative_path)>=LFN_FATFS_PATH||!session||!file||!seed||seed==UINT64_MAX||!sync||priority<1||priority>=6||!xTaskGetTickCount())return false;
 if(!claim_start())return false;
 atomic_store(&sd_mode,true);memset(sd_relative_path,0,sizeof(sd_relative_path));memcpy(sd_relative_path,relative_path,strlen(relative_path)+1);
 session_id=session;file_id=file;sequence_seed=seed;sync_period=sync;return spawn_task(priority);
}
static StoragePump*pump(void){return atomic_load_explicit(&ready,memory_order_acquire)?&lfn_storage_resources.pump:NULL;}
bool lfn_storage_task_connect_capture(void){
 if(!pump())return false;
 bool expected=false;if(!atomic_compare_exchange_strong(&connection_requested,&expected,true))return false;
 if(lfn_capture_image_storage_attach(&lfn_storage_resources.route))return true;
 atomic_store(&connection_requested,false);return false;
}
bool lfn_storage_task_source_facts(CaptureRouteFacts*out){return pump()&&capture_storage_route_facts_copy(&lfn_storage_resources.route,out);}
StorageResult lfn_storage_task_metadata(const RecordIdentity*i,const uint8_t*b,size_t n,StoragePumpTicket*t){StoragePump*p=pump();return p?storage_pump_metadata(p,i,b,n,t):STORE_BUSY;}
StorageResult lfn_storage_task_energy(const LfnServiceMessage*m,StoragePumpTicket*t){StoragePump*p=pump();return p?storage_pump_energy(p,m,t):STORE_BUSY;}
StorageResult lfn_storage_task_summary(const MinuteSummary*s,StoragePumpTicket*t){StoragePump*p=pump();return p?storage_pump_summary(p,s,t):STORE_BUSY;}
StorageResult lfn_storage_task_psd(const LfnSpectrum*s,StoragePumpTicket*t){StoragePump*p=pump();return p?storage_pump_psd(p,s,t):STORE_BUSY;}
StorageResult lfn_storage_task_pcm(uint64_t session,const uint8_t*b,size_t n,StoragePumpTicket*t){StoragePump*p=pump();return p?storage_pump_pcm(p,session,b,n,t):STORE_BUSY;}
StorageResult lfn_storage_task_poll(StoragePumpTicket t,StoragePumpResult*out){StoragePump*p=pump();return p?storage_pump_poll(p,t,out):STORE_BUSY;}
StorageResult lfn_storage_task_ack(StoragePumpTicket t){StoragePump*p=pump();return p?storage_pump_ack(p,t):STORE_BUSY;}
StorageResult lfn_storage_task_abort(StoragePumpTicket t){StoragePump*p=pump();return p?storage_pump_abort(p,t):STORE_BUSY;}
StorageResult lfn_storage_task_close(void){
#ifdef LFN_STORAGE_OWNER_INSTANCE
 if(atomic_load_explicit(&mf_mode,memory_order_acquire)){atomic_store_explicit(&sd_close_requested,true,memory_order_release);return STORE_OK;}
#endif
 if(atomic_load_explicit(&sd_recovery_only,memory_order_acquire)){atomic_store_explicit(&sd_close_requested,true,memory_order_release);return STORE_OK;}
 StoragePump*p=pump();if(!p)return STORE_BUSY;
 if(atomic_load_explicit(&connection_requested,memory_order_acquire))return lfn_capture_image_storage_close()?STORE_OK:STORE_BUSY;
 StorageResult r=storage_pump_close(p);
 if(r==STORE_OK)atomic_store_explicit(&sd_close_requested,true,memory_order_release);
 return r;
}
StorageResult lfn_storage_task_recover(bool repair){StoragePump*p=pump();return p?storage_pump_recover(p,repair):STORE_BUSY;}
bool lfn_storage_task_sd_closed(void){return atomic_load_explicit(&sd_mode,memory_order_acquire)&&atomic_load_explicit(&sd_closed,memory_order_acquire);}
bool lfn_storage_task_health(StorageTaskHealth*out){
 if(!out)return false;
 StorageTaskHealth h={0};
 h.sd_mode=atomic_load_explicit(&sd_mode,memory_order_acquire);h.sd_stage=atomic_load_explicit(&sd_stage,memory_order_acquire);h.sd_last_error=atomic_load_explicit(&sd_last_error,memory_order_acquire);
 h.loop_steps=atomic_load_explicit(&loop_steps,memory_order_acquire);h.loop_stage=atomic_load_explicit(&loop_stage,memory_order_acquire);h.pump_stage=storage_pump_debug_stage();
 h.pre_writer_state=(unsigned)lfn_storage_resources.writer.state;h.pre_writer_error=(unsigned)lfn_storage_resources.writer.error;h.pre_tail_reason=(int)lfn_storage_resources.writer.tail_reason;
 h.recovered_records=atomic_load_explicit(&sd_recovered,memory_order_acquire);h.file_prefix=atomic_load_explicit(&sd_prefix,memory_order_acquire);h.file_bytes=atomic_load_explicit(&sd_file_bytes,memory_order_acquire);h.tail_repaired=atomic_load_explicit(&sd_tail_repaired,memory_order_acquire);h.tail_reason=atomic_load_explicit(&sd_tail_reason,memory_order_acquire);
 h.service_pending=lfn_storage_resources.service.health.pending;h.sealed=lfn_storage_resources.service.health.sealed;h.drained=lfn_storage_resources.service.health.drained;h.fatal=lfn_storage_resources.service.health.fatal;h.service_pending_result=(int)lfn_storage_resources.service.health.pending_result;h.service_accepted=lfn_storage_resources.service.health.accepted;
 h.mounted=atomic_load_explicit(&sd_mounted,memory_order_acquire);
 h.closed=atomic_load_explicit(&sd_closed,memory_order_acquire);h.close_failed=atomic_load_explicit(&sd_close_failed,memory_order_acquire);
 h.sd_fault=(StorageIoResult)atomic_load_explicit(&sd_fault,memory_order_acquire);
 h.started=atomic_load_explicit(&started,memory_order_acquire);h.ready=atomic_load_explicit(&ready,memory_order_acquire);h.startup_failed=atomic_load_explicit(&failed,memory_order_acquire);
 if(h.ready){
  if(!atomic_load_explicit(&sd_recovery_only,memory_order_acquire)){
   if(storage_pump_health(pump(),&h.pump)!=STORE_OK)return false;
   if(atomic_load_explicit(&lfn_storage_resources.route.attached,memory_order_acquire)&&!capture_storage_route_health(&lfn_storage_resources.route,&h.route))return false;
  }
  TaskStatus_t status;vTaskGetInfo(owner,&status,pdTRUE,eInvalid);h.stack_unused_words=status.usStackHighWaterMark;h.free_heap=xPortGetFreeHeapSize();}
 *out=h;return true;
}
#ifdef LFN_STORAGE_OWNER_INSTANCE
bool lfn_storage_task_start_sd_multifile(uint64_t session,uint64_t seed,uint64_t sync,unsigned priority,const char *root){
 if(!session||!seed||seed==UINT64_MAX||!sync||priority<1||priority>=6||!root||!*root||strlen(root)>=sizeof(mf_root))return false;
 if(!claim_start())return false;
 mf_session=session;mf_seed=seed;mf_sync=sync;
 memset(mf_root,0,sizeof(mf_root));memcpy(mf_root,root,strlen(root)+1);
 atomic_store(&sd_mode,false);atomic_store(&mf_mode,true);
 return spawn_task(priority);
}
bool lfn_storage_task_start_sd_write(uint64_t session,uint64_t seed,uint64_t sync,unsigned priority,const char *root,unsigned target){
 if(!session||!seed||seed==UINT64_MAX||!sync||priority<1||priority>=6||!root||!*root||strlen(root)>=sizeof(mf_root)||!target)return false;
 if(!claim_start())return false;
 mf_session=session;mf_seed=seed;mf_sync=sync;
 memset(mf_root,0,sizeof(mf_root));memcpy(mf_root,root,strlen(root)+1);
 atomic_store(&mf_write_mode,true);atomic_store(&mf_write_target,target);
 atomic_store(&sd_mode,false);atomic_store(&mf_mode,true);
 return spawn_task(priority);
}
bool lfn_storage_task_owner_health(StorageTaskOwnerHealth *out){
 if(!out)return false;
 memset(out,0,sizeof(*out));
 out->started=atomic_load_explicit(&started,memory_order_acquire);
 out->mode=atomic_load_explicit(&mf_mode,memory_order_acquire);
 out->ready=atomic_load_explicit(&mf_ready,memory_order_acquire);
 out->failed=atomic_load_explicit(&mf_failed,memory_order_acquire);
 out->closed=atomic_load_explicit(&sd_closed,memory_order_acquire);
 out->owner_state=atomic_load_explicit(&mf_owner_state,memory_order_acquire);
 out->owner_error=atomic_load_explicit(&mf_owner_error,memory_order_acquire);
 out->steps=atomic_load_explicit(&mf_steps,memory_order_acquire);
 out->phase=atomic_load_explicit(&mf_phase,memory_order_acquire);
 out->written=atomic_load_explicit(&mf_written,memory_order_acquire);
 out->write_target=atomic_load_explicit(&mf_write_target,memory_order_acquire);
 out->sd_stage=atomic_load_explicit(&sd_stage,memory_order_acquire);
 out->last_error=atomic_load_explicit(&sd_last_error,memory_order_acquire);
 {
  StorageOwnerGroup *g=&lfn_storage_resources.owner.workspace.groups[lfn_storage_resources.owner.group];
  out->epoch_state=(unsigned)g->capture_epoch.state;
  out->lifecycle_state=(unsigned)g->lifecycle.state;
  out->rotation_state=(unsigned)lfn_storage_resources.owner.rotation.state;
  out->route_attached=atomic_load(&lfn_storage_resources.owner.workspace.route.attached)?1u:0u;
  out->route_generation=lfn_storage_resources.owner.workspace.route.published.generation;
 }
 out->sd_fault=atomic_load_explicit(&sd_fault,memory_order_acquire);
 return true;
}
#endif