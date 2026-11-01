#include "event_fixture_image.h"
#include "event_fixture.h"
#include "event_runtime.h"
#include "storage_owner.h"
#include "../storage/fatfs_file_port.h"
#include "../storage/fatfs_namespace_port.h"
#include "../time/timer_scale.h"
#include "FreeRTOS.h"
#include "task.h"
#include "xiltimer.h"
#include "diskio.h"
#include <stdio.h>
#include <string.h>

#if !defined(LFN_EVENT_FIXTURE) || LFN_STORAGE_OWNER_FILES != 4
#error Require independent four-file event fixture image
#endif
#define DATA __attribute__((section(".lfn_data.event_fixture"),aligned(64),used))
enum { FX_STACK_WORDS=6144,FX_ORIGIN=31 };
static StorageOwnerGroup group DATA;
static StorageFileHandles handles DATA;
static StorageNamespace space;
static StorageEventRoll roll DATA;
static PcmHistory history DATA;
static EventRuntime events DATA;
static EventFixture source DATA;
static CaptureStorageRoute route DATA;
static LfnServiceBridge bridge DATA;
static StorageMetadataBuilder builder DATA;
static uint8_t metadata[STORAGE_METADATA_BYTES] DATA;
static LfnFatFsVolume volume;
static LfnFatFsFilePort files;
static LfnFatFsNamespacePort guard;
static StorageFactoryConfig factory_config DATA;
static TaskHandle_t task;
static uint64_t session,lease,started_us;
static bool source_done,route_ready,close_frozen,definition_pending;
static unsigned slots[6],definition_file,definition_kind;
static StorageTicket definition_ticket;
static char root[16],paths[6][STORAGE_HANDLE_PATH];
EventFixtureImageHealth lfn_event_fixture_health;
static EventFixtureImageHealth current;
const uint32_t lfn_event_fixture_sizes[5]={sizeof(group)+sizeof(handles)+sizeof(roll)+sizeof(route)+sizeof(bridge)+sizeof(builder)+sizeof(metadata)+sizeof(factory_config),sizeof(history),sizeof(events),sizeof(source),FX_STACK_WORDS*sizeof(StackType_t)};
static const char *const names[6]={"AMON.LFR","ARAW.LFR","ASUM.LFR","AEVT.LFR","AIDX.SFL","ADIR.SFC"};
static const char device[]="PS_SYNTHETIC_FIXTURE";
static const char hash[]="0000000000000000000000000000000000000000000000000000000000000000";
static uint64_t now_us(void){XTime now;XTime_GetTime(&now);return lfn_timer_ticks_us(now,COUNTS_PER_SECOND);}
static bool owner(void*c){(void)c;return task&&xTaskGetCurrentTaskHandle()==task;}
static bool quiescent(void*c){(void)c;return !route_ready||source_done;}
static StorageIoResult media(void*c){(void)c;return disk_ioctl(0,CTRL_SYNC,NULL)==RES_OK?STORE_IO_OK:STORE_IO_ERROR;}
static void fail(unsigned code){current.error=code;current.failed=true;current.running=false;}
static StorageResult close_epoch(void*c,uint64_t id){
 (void)c;
 if(!close_frozen){StorageResult r=storage_file_handles_freeze_epoch(&handles,&group.catalog,slots);if(r!=STORE_OK)return r;close_frozen=true;}
 for(unsigned k=0;k<6;k++)if(handles.files[slots[k]].spec.file_id==id)return storage_file_handles_close(&handles,slots[k]);
 return STORE_INVALID;
}
static StorageIoResult roll_mkdir(void*c,const char*path){(void)c;return lfn_fatfs_mkdir(&volume,path,true);}
static StorageResult roll_create(void*c,StorageFileHandles*h,unsigned slot){(void)c;return lfn_fatfs_file_port_create(&files,h,slot);}
static StorageIoResult roll_media(void*c,StorageFileHandles*h){(void)c;(void)h;return media(NULL);}
static bool roll_observe(void*c,StorageService*s,uint64_t id){(void)c;return event_runtime_bind_file(&events,s,id);}
static StorageResult begin_roll(void*c,uint64_t ordinal,uint64_t now){
 (void)c;StorageEventRollOps ops={NULL,roll_mkdir,roll_create,roll_media,roll_observe};
 return storage_event_roll_request(&roll,&group.factory,&handles,&space,&ops,3,slots[3],paths[5],ordinal,now);
}
static StorageResult roll_status(void*c,uint64_t*id){(void)c;return storage_event_roll_status(&roll,id);}
static bool pcm(void*c,const LfnServiceIdentity*id,const PacketView*packet){
 (void)c;HistoryStatus h=history_append_packet(&history,id->session_id,packet);
 if(h==HISTORY_INVALID){fail(41);return false;}
 /* A full route records its own loss; that attempted candidate is consumed. */
 (void)capture_storage_route_packet(&route,id,packet);return true;
}
static bool energy(void*c,const LfnServiceIdentity*id,const CaptureEnergyMessage*message){
 (void)c;LfnServiceResult r=lfn_service_energy_offer(&bridge,id,message);
 if(r==SERVICE_INVALID||r==SERVICE_EXHAUSTED){fail(42);return false;}
 return true;
}
static bool setup(void){
 current.stage=1;
 if(!lfn_fatfs_volume_init(&volume,"0:",owner,NULL)||lfn_fatfs_mount(&volume)!=STORE_IO_OK)return false;
 if(lfn_fatfs_mkdir(&volume,root,false)!=STORE_IO_OK||lfn_fatfs_unmount(&volume)!=STORE_IO_OK)return false;
 current.stage=2;
 if(!storage_namespace_init(&space,root,1,owner,NULL))return false;
 LfnFatFsNamespaceName allowed[6]={0};
 for(unsigned k=0;k<6;k++){strcpy(allowed[k].name,names[k]);allowed[k].snapshot_metadata=k==5;}
 if(!lfn_fatfs_namespace_port_bind(&guard,&volume,&space,allowed,6)||storage_namespace_write_begin(&space,&lease)!=STORE_OK||lfn_fatfs_mount(&volume)!=STORE_IO_OK)return false;
 StorageFileOps ops;if(!lfn_fatfs_file_port_init(&files,&volume,media,NULL,&ops)||!storage_file_handles_init(&handles,&ops))return false;
 uint64_t base=(session&UINT64_C(0xFFFFFFFFFFFF))<<16;
 for(unsigned k=0;k<6;k++){
  current.stage=3+k;
  (void)snprintf(paths[k],sizeof(paths[k]),"%s/%s",root,names[k]);
  StorageHandleSpec spec={.session_id=session,.file_id=base+k+1,.catalog_id=base+6,.ledger_id=base+5,.order_key=k+1,
   .kind=k<4?HANDLE_DATA:HANDLE_AUXILIARY,.family=k==1?RETAIN_PSD:k==2?RETAIN_SUMMARY:k==3?RETAIN_PCM:RETAIN_METADATA};
  strcpy(spec.path,paths[k]);
  if(storage_file_handles_register(&handles,&spec,&slots[k])!=STORE_OK||lfn_fatfs_file_port_create(&files,&handles,slots[k])!=STORE_OK)return false;
 }
 StorageFactoryConfig *f=&factory_config;memset(f,0,sizeof(*f));
 f->session_id=session;f->next_sequence=1;f->ledger_id=base+5;f->catalog_id=base+6;f->sync_us=1000000;f->created_us=now_us();
 f->max_file_bytes=UINT64_MAX;f->max_file_age_us=UINT64_MAX;f->count=4;
 f->ledger=handles.files[slots[4]].backend;f->manifest=handles.files[slots[5]].backend;
 f->arena=group.arena;f->pool=&group.pool;f->family=&group.family;f->owner=&group.family_queue;f->catalog=&group.catalog;
 f->verifier=&group.verifier;f->epoch=&group.epoch;f->engine=&group.engine;f->hooks=(StorageEpochHooks){NULL,quiescent,close_epoch};f->is_owner=owner;
 for(unsigned k=0;k<4;k++){
  StorageFactoryFile *a=&f->files[k];a->raw=handles.files[slots[k]].backend;a->descriptor.file_id=base+k+1;
  a->descriptor.role=k==3?CATALOG_EVENT:k==1?CATALOG_RAW:CATALOG_MONITOR;strcpy(a->descriptor.name,names[k]);
  a->binding=&group.bindings[k];a->writer=&group.writers[k];a->queue=&group.queues[k];a->service=&group.services[k];a->pump=&group.pumps[k];a->plan=&group.plans[k];a->config_id=1;a->capture_id=1;
 }
 current.stage=10;return storage_factory_init(&group.factory,f);
}
static bool definition(StorageRequest *request){
 StorageSessionFacts sf={device,session,0,now_us()*1000,hash,true};
 if(definition_kind==0)return storage_build_session(&builder,&sf,request,metadata,sizeof(metadata));
 if(definition_kind==1){
  StorageConfigFacts cfg={0};cfg.device_id=device;cfg.session_id=session;cfg.config_hash=hash;
  cfg.hardware_id=device;cfg.gain_setting_id="UNVERIFIED";cfg.adc_id="NONE";cfg.microphone_id="NONE";cfg.point_pose_id="NONE";cfg.band_definition_id="LFN_THIRD_OCTAVE";
  cfg.config=(PlCaptureConfig){.source=1,.config_id=1};cfg.event=(StorageEventFacts){6,3,60,5,20,5e-10,3,16};
  return storage_build_config(&builder,&cfg,request,metadata,sizeof(metadata));
 }
 /* This is an explicit PS simulation contract, not a fabricated PL snapshot.
  * Legacy mapping is unconfirmed; no NOMINAL_PAIRED/TEST_COUNTER_CROSSING. */
 int n=snprintf((char*)metadata,sizeof(metadata),
  "{\"schema_version\":1,\"device_id\":\"PS_SYNTHETIC_FIXTURE\",\"session_id\":\"%llu\",\"capture_id\":\"1\",\"config_id\":1,"
  "\"capture_revision\":\"1\",\"state\":\"OPEN\",\"mode\":0,\"source\":1,\"channel\":0,\"pattern\":0,"
  "\"origin_raw\":\"31\",\"first_valid_raw\":\"31\",\"bound_mask\":11,\"first_stream_seq\":[\"0\",\"0\",null,\"0\"],"
  "\"first_candidate_raw\":[\"31\",\"31\",null,\"31\"],\"start_monotonic_ns\":\"0\",\"mono_anchor_raw\":\"31\","
  "\"mono_anchor_ns\":\"0\",\"mono_ns_per_raw\":20833.333333333332,\"mono_uncertainty_ns\":\"18446744073709551615\","
  "\"end_raw\":null,\"end_monotonic_ns\":null,\"end_reason\":null,\"provenance\":\"PS_SYNTHETIC_FIXTURE\","
  "\"time_quality\":\"UNCONFIRMED\",\"spectrum_evidence\":\"NOT_GENERATED\"}",(unsigned long long)session);
 if(n<=0||(size_t)n>=sizeof(metadata))return false;
 *request=(StorageRequest){.identity={RECORD_CAPTURE,0,1,session,1,FX_ORIGIN,FX_ORIGIN,0},.payload=metadata,.payload_bytes=(size_t)n};return true;
}
static void definitions(void){
 StorageService *service=&group.services[definition_file];
 if(!definition_pending){
  StorageRequest request;if(!definition(&request)){fail(20);return;}
  StorageSubmit s=storage_service_metadata(service,&request.identity,request.payload,request.payload_bytes);
  if(s.result==STORE_BUSY||s.result==STORE_CAPACITY)return;
  if(s.result!=STORE_OK){fail(21u+s.result);return;}definition_ticket=s.ticket;definition_pending=true;return;
 }
 StorageReceipt receipt;StorageResult r=storage_queue_poll(service->queue,definition_ticket,&receipt);
 if(r==STORE_BUSY)return;
 if(r!=STORE_OK||receipt.state==STORE_FAILED){fail(30);return;}
 if(receipt.state!=STORE_SYNCED)return;
 bool observed=false;for(unsigned k=0;k<STORAGE_RECEIPTS;k++)if(service->slots[k].occupied&&service->slots[k].ticket.slot==definition_ticket.slot&&service->slots[k].ticket.generation==definition_ticket.generation)observed=service->slots[k].observed;
 if(!observed)return;
 definition_pending=false;if(++definition_file==4){definition_file=0;definition_kind++;}
}
static bool begin_source(uint64_t now){
 LfnServiceIdentity id={.session_id=session,.capture_id=1,.config={.source=1,.config_id=1}};
 EventFixtureHooks hooks={NULL,pcm,energy};StorageQueueLocks locks={0};LfnServiceLocks bridge_locks={0};
 StorageMultiTargets targets={&group.pumps[0],&group.pumps[1],&group.pumps[0],&group.pumps[1]};CaptureStorageSink sink;
 StoragePump *pumps[]={&group.pumps[0],&group.pumps[1],&group.pumps[2],&group.pumps[3]};
 EventRuntimeRollOps rolls={NULL,begin_roll,roll_status};history_init(&history);
 if(!history_begin(&history,(HistoryIdentity){session,1,1},true)||!lfn_service_bridge_init(&bridge,&bridge_locks)||
  !storage_multi_sink_init(&group.sink,session,1,&targets,&sink)||!storage_multi_sink_set_close_pumps(&group.sink,pumps,4)||
  !capture_storage_route_init(&route,session,&locks,&sink)||!capture_storage_route_bind(&route,&bridge)||
  !event_runtime_init(&events,session,&history,NULL,&rolls)||!event_runtime_attach(&events,&group.sink,&route,pumps,4,1)||
  !event_fixture_init(&source,&id,now,FX_ORIGIN,&hooks))return false;
 if(storage_namespace_write_end(&space,lease)!=STORE_OK)return false;
 lease=0;route_ready=true;started_us=now;current.running=true;return true;
}
static void publish(uint64_t now){
 EventFixtureImageHealth h=current;
 h.elapsed_us=started_us?now-started_us:0;h.samples=source.next_sample;h.pcm_packets=source.pcm_count;h.energy_windows=source.energy_count;
 h.events=events.events_closed;h.segments=events.segments_closed;h.event_file_id=group.queues[3].file_id;
 h.mon_synced=group.services[0].health.synced;h.event_synced=group.services[3].health.synced;
 h.roll_state=roll.state;h.roll_error=roll.error;h.factory_state=group.factory.state;h.engine_state=group.engine.state;
 /* BSP does not export the optional watermark API; JTAG checks the real stack. */
 h.stack_free_words=UINT32_MAX;
 if(route_ready&&!source_done&&h.elapsed_us>source.next_sample*1000){uint64_t lag=h.elapsed_us-source.next_sample*1000;if(lag>h.max_source_lag_us)h.max_source_lag_us=lag;}
 current=h;taskENTER_CRITICAL();lfn_event_fixture_health=h;taskEXIT_CRITICAL();
}
static void run(void *unused){
 (void)unused;
 if(!setup()){fail(10+current.stage);goto finished;}
 for(;;){
  uint64_t now=now_us();
  if(!route_ready){
   storage_factory_step(&group.factory,now);
   if(group.factory.state==FACTORY_READY){
    current.stage=11;
    if(definition_kind<3)definitions();else if(!begin_source(now))fail(40);else current.stage=12;
   }
  }else{
   if(!source_done){
    EventFixtureResult r=event_fixture_step(&source,now);
    if(r==FIXTURE_ERROR)fail(43);
    else if(r==FIXTURE_DONE){source_done=true;current.stage=13;}
   }
   if(source_done){
    (void)lfn_service_flush(&bridge);capture_storage_route_flush(&route);
    if(!route.published.producer_closed)(void)capture_storage_route_producer_close(&route);
   }
   capture_storage_route_step(&route);event_runtime_step(&events,now);
   if(storage_event_roll_active(&roll))storage_event_roll_step(&roll,now);else storage_factory_step(&group.factory,now);
   if(events.error||roll.state==EVENT_ROLL_FAILED)fail(50u+events.error);
   CaptureRouteHealth rh;
   if(capture_storage_route_health(&route,&rh)&&rh.consumer.blocked)fail(60u+rh.consumer.error);
   if(source_done&&capture_storage_route_health(&route,&rh)&&rh.consumer.handoff_drained&&!storage_event_roll_active(&roll)&&group.engine.state==ENGINE_ACTIVE){
    if(storage_namespace_write_begin(&space,&lease)!=STORE_OK||storage_engine_request_rotation(&group.engine)!=STORE_OK)fail(70);
    else current.stage=14;
   }
   if(group.engine.state==ENGINE_HANDOFF){
    if(storage_file_handles_finish_epoch(&handles,&group.catalog,slots)!=STORE_OK||media(NULL)!=STORE_IO_OK||lfn_fatfs_unmount(&volume)!=STORE_IO_OK||storage_namespace_write_end(&space,lease)!=STORE_OK)fail(71);
    else {lease=0;current.stage=15;current.closed=true;current.running=false;}
   }
  }
  if(group.factory.state==FACTORY_FAILED||group.engine.state==ENGINE_FAILED)fail(80u+group.factory.error);
  publish(now);
  if(current.failed||current.closed)break;
  /* Final stopped seal scans can batch; RUN always yields each bounded step. */
  static unsigned batch;
  if(source_done&&group.engine.state==ENGINE_ROTATING&&++batch<32)continue;
  batch=0;vTaskDelay(1);
 }
finished:
 publish(now_us());for(;;)vTaskDelay(pdMS_TO_TICKS(1000));
}
bool lfn_event_fixture_image_start(uint64_t token){
 if(!token||current.started)return false;
 /* NOLOAD .lfn_data.event_fixture survives warm JTAG reloads in DDR; a stale
  * failed roll/engine state must never leak into a new fixture session. */
 memset(&group,0,sizeof(group));memset(&handles,0,sizeof(handles));memset(&space,0,sizeof(space));
 memset(&roll,0,sizeof(roll));memset(&history,0,sizeof(history));memset(&events,0,sizeof(events));
 memset(&source,0,sizeof(source));memset(&route,0,sizeof(route));memset(&bridge,0,sizeof(bridge));
 memset(&builder,0,sizeof(builder));memset(metadata,0,sizeof(metadata));
 memset(&volume,0,sizeof(volume));memset(&files,0,sizeof(files));memset(&guard,0,sizeof(guard));
 memset(&factory_config,0,sizeof(factory_config));
 session=0;lease=0;started_us=0;source_done=false;route_ready=false;close_frozen=false;definition_pending=false;
 memset(slots,0,sizeof(slots));definition_file=0;definition_kind=0;memset(&definition_ticket,0,sizeof(definition_ticket));
 memset(root,0,sizeof(root));memset(paths,0,sizeof(paths));
 memset(&current,0,sizeof(current));session=token;
 (void)snprintf(root,sizeof(root),"FX%06X",(unsigned)(token&0xFFFFFF));
 current.session=token;strcpy(current.directory,root);current.started=true;
 taskENTER_CRITICAL();lfn_event_fixture_health=current;taskEXIT_CRITICAL();
 if(xTaskCreate(run,"event_fx",FX_STACK_WORDS,NULL,tskIDLE_PRIORITY+1,&task)!=pdPASS){
  fail(1);taskENTER_CRITICAL();lfn_event_fixture_health=current;taskEXIT_CRITICAL();return false;
 }
 return true;
}
bool lfn_event_fixture_image_health(EventFixtureImageHealth *out){
 if(!out)return false;
 taskENTER_CRITICAL();*out=lfn_event_fixture_health;taskEXIT_CRITICAL();return true;
}
