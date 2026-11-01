#include "sd_rec.h"
#include "../storage/storage_family.h"
#ifdef LFN_SD_PROBE
#include "sd_probe.h"
#endif
#include "../storage/storage_task_freertos.h"
#include "../storage/metadata_builder.h"
#include "service_bridge.h"
#include "../dsp/spectrum.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
enum { SD_REC_WAIT_MS=20000, SD_REC_SYNC_MS=60000, SD_REC_CLOSE_MS=60000, SD_REC_RECORDS=5 };
static StorageMetadataBuilder builder;
static uint8_t payload[STORAGE_METADATA_BYTES];
static LfnSpectrum spectrum;
static uint64_t cfg_session;
static PlCaptureConfig cfg_config;
static const char *hash64="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static void say(void (*emit)(const char *),const char *fmt,...){
 char line[200];va_list ap;va_start(ap,fmt);int n=vsnprintf(line,sizeof(line),fmt,ap);va_end(ap);
 if(n>0&&n<(int)sizeof(line))(void)emit(line);
}
/* 100 Hz tick: sub-10 ms pdMS_TO_TICKS values become 0, which does not block
 * this task and would starve the lower-priority storage task. */
static TickType_t dly(unsigned ms){TickType_t t=pdMS_TO_TICKS(ms);return t?t:1;}
static uint64_t mono_ns(void){return (uint64_t)xTaskGetTickCount()*1000000ull+1000000000ull;}
static bool wait_ready(StorageTaskHealth*h){
 for(unsigned i=0;i<SD_REC_WAIT_MS/10;i++){
  if(!lfn_storage_task_health(h))return false;
  if(h->ready)return true;
  if(h->startup_failed)return false;
  vTaskDelay(dly(10));
 }
 return false;
}
static bool wait_ticket(StoragePumpTicket t,void (*emit)(const char *),const char *tag){
 StoragePumpResult r;StorageResult s=STORE_BUSY;
 for(unsigned i=0;i<SD_REC_WAIT_MS/5;i++){
  s=lfn_storage_task_poll(t,&r);
  if(s==STORE_OK&&r.done){
   StorageResult a=lfn_storage_task_ack(t);
   if(a==STORE_OK)return true;
   say(emit,"LFN SDR %s ack=%d\r\n",tag,(int)a);return false;
  }
  if(s==STORE_EXPIRED){say(emit,"LFN SDR %s poll=expired\r\n",tag);return false;}
  vTaskDelay(dly(5));
 }
 StorageTaskHealth hh;memset(&hh,0,sizeof(hh));
 (void)lfn_storage_task_health(&hh);
 say(emit,"LFN SDR %s poll=%d done=%d last=%d pieces=%u steps=%llu acc=%llu comp=%llu occ=%u fatal=%d closed=%d writer=%u\r\n",
     tag,(int)s,r.done?1:0,(int)r.last_submit.result,r.pieces,
     (unsigned long long)hh.pump.steps,(unsigned long long)hh.pump.accepted,(unsigned long long)hh.pump.completed,
     hh.pump.occupied,hh.pump.fatal?1:0,hh.pump.closed?1:0,(unsigned)hh.pump.writer_state);
 say(emit,"LFN SDR %s loop=%u stage=%u pump_stage=%u\r\n",tag,hh.loop_steps,hh.loop_stage,hh.pump_stage);
 return false;
}
static bool metadata_wait(StorageRequest*r,void (*emit)(const char *),const char *tag){
 StoragePumpTicket t;
 StorageResult s=lfn_storage_task_metadata(&r->identity,r->payload,r->payload_bytes,&t);
 if(s!=STORE_OK){say(emit,"LFN SDR %s submit=%d\r\n",tag,(int)s);return false;}
 return wait_ticket(t,emit,tag);
}
static bool wait_synced(uint64_t target,void (*emit)(const char *),const char *tag){
 for(unsigned i=0;i<SD_REC_SYNC_MS/50;i++){
  StorageTaskHealth h;
  if(!lfn_storage_task_health(&h))return false;
  if(h.pump.fatal||h.startup_failed)return false;
  if(h.pump.synced>=target)return true;
  vTaskDelay(dly(50));
 }
 StorageTaskHealth h;memset(&h,0,sizeof(h));if(!lfn_storage_task_health(&h))return false;
 say(emit,"LFN SDR %s wait=fail synced=%llu failed=%llu comp=%llu acc=%llu writer=%u werr=%u unsaved=%u inflight=%u steps=%llu\r\n",
     tag,(unsigned long long)h.pump.synced,(unsigned long long)h.pump.failed,
     (unsigned long long)h.pump.completed,(unsigned long long)h.pump.accepted,
     (unsigned)h.pump.writer_state,(unsigned)h.pump.writer_error,
     h.pump.unsaved_ranges,h.pump.inflight_ranges,(unsigned long long)h.pump.steps);
 say(emit,"LFN SDR %s svc pending=%d pres=%d sealed=%d drained=%d fatal=%d acc=%llu\r\n",tag,h.service_pending?1:0,h.service_pending_result,h.sealed?1:0,h.drained?1:0,h.fatal?1:0,(unsigned long long)h.service_accepted);
 return false;
}
bool lfn_sd_rec_run(void (*emit)(const char *line)){
 if(!emit)return false;
 unsigned token=(unsigned)(xTaskGetTickCount()*2654435761u)&0xFFFFFu;
 char name[16];
 (void)snprintf(name,sizeof(name),"LFR%05X.LFR",token);
 uint64_t session=(((uint64_t)token)<<32)|UINT64_C(0x5A5A5A5A);
 if(!lfn_storage_task_start_sd(session,session^UINT64_C(0x9E3779B97F4A7C15),UINT64_C(12345),UINT64_C(5000000),3,name)){
  say(emit,"LFN SDR START result=fail name=%s\r\n",name);
  say(emit,"LFN SDR DONE result=fail\r\n");
  return false;
 }
 say(emit,"LFN SDR START result=ok name=%s session=%llu\r\n",name,(unsigned long long)session);
 StorageTaskHealth h;
 if(!wait_ready(&h)){
  say(emit,"LFN SDR READY result=fail startup_failed=%d sd_fault=%d sd_stage=%u sd_last_fr=%u\r\n",h.startup_failed?1:0,(int)h.sd_fault,(unsigned)h.sd_stage,h.sd_last_error);
  say(emit,"LFN SDR DONE result=fail\r\n");
  return false;
 }
 say(emit,"LFN SDR READY result=ok mounted=%d\r\n",h.mounted?1:0);
 cfg_session=session;cfg_config=(PlCaptureConfig){0,PL_CAPTURE_TEST,0,3,1};
 StorageRequest r;
 StorageSessionFacts sf={"LFN_AX7021",session,0,mono_ns(),hash64,true};
 bool b1=storage_build_session(&builder,&sf,&r,payload,sizeof(payload));
 if(!b1||!metadata_wait(&r,emit,"SESSION")){
  say(emit,"LFN SDR META session=fail built=%d\r\n",b1?1:0);goto fail;
 }
 say(emit,"LFN SDR META session=ok\r\n");
 StorageConfigFacts cf={0};
 cf.device_id="LFN_AX7021";cf.config_hash=hash64;cf.hardware_id="AX7021_PRE_ADC";
 cf.gain_setting_id="UNVERIFIED";cf.adc_id="NONE";cf.microphone_id="NONE";
 cf.point_pose_id="NONE";cf.band_definition_id="LFN_THIRD_OCTAVE";
 cf.session_id=session;cf.calibration_id=0;cf.config=cfg_config;
 cf.gain_verified=false;cf.fs_measured=false;cf.gain_db=0;cf.measured_raw_fs_hz=0;cf.fs_uncertainty_ppm=0;
 cf.event=(StorageEventFacts){6,3,60,5,20,5e-10,3,16};
 bool b2=storage_build_config(&builder,&cf,&r,payload,sizeof(payload));
 if(!b2||!metadata_wait(&r,emit,"CONFIG")){
  say(emit,"LFN SDR META config=fail built=%d\r\n",b2?1:0);goto fail;
 }
 say(emit,"LFN SDR META config=ok\r\n");
 StorageCaptureFacts cap={0};
 cap.device_id="LFN_AX7021";cap.config=cfg_config;cap.max_age_ticks=100;cap.revision=1;
 cap.start_ns=mono_ns();cap.mono_ns=cap.start_ns;cap.ns_per_raw=1e9/48000.0;cap.uncertainty_ns=10000000;
 cap.started.seq=2;cap.started.flags=PL_CAPTURE_F_RUNNING|PL_CAPTURE_F_POSITION_VALID|PL_CAPTURE_F_TEST;
 cap.started.capture_id=1;cap.started.active_config_id=1;cap.started.configured=cfg_config;
 cap.active.seq=2;cap.active.kind=PL_CONTRACT_ACTIVE;cap.active.mode=cfg_config.mode;
 cap.active.source=cfg_config.source;cap.active.channel=cfg_config.channel;cap.active.pattern=cfg_config.pattern;
 cap.active.bound_mask=cfg_config.mode?4:11;
 cap.active.plan=(CapturePlan){session,1,1,false,{5,6,7,8}};
 cap.first_valid_raw=1679;cap.mono_raw=cap.first_valid_raw;
 for(unsigned k=0;k<4;k++)if(cap.active.bound_mask&(1u<<k))
  cap.active.anchors[k]=(CaptureAnchor){session,1,1,k+1,cap.active.plan.first_seq[k],cap.first_valid_raw};
 bool b3=storage_build_capture(&builder,&cap,&r,payload,sizeof(payload));
 if(!b3||!metadata_wait(&r,emit,"CAPTURE")){
  say(emit,"LFN SDR META capture=fail built=%d\r\n",b3?1:0);goto fail;
 }
 say(emit,"LFN SDR META capture=ok\r\n");
 if(!wait_synced(3,emit,"SYNC-DEF")){say(emit,"LFN SDR SYNC definitions=fail\r\n");goto fail;}
 say(emit,"LFN SDR SYNC definitions=ok\r\n");
 {
  LfnServiceMessage m;memset(&m,0,sizeof(m));
  m.kind=SERVICE_ENERGY;m.identity=(LfnServiceIdentity){session,1,cfg_config};
  m.first_delivery=1;m.end_delivery=2;
  m.energy.gap=false;m.energy.record=(PlEnergy){8,1,1679,1000,1,LFN_VALID};
  StoragePumpTicket t;
  StorageResult es=lfn_storage_task_energy(&m,&t);
  if(es!=STORE_OK||!wait_ticket(t,emit,"ENERGY")){
   say(emit,"LFN SDR ENERGY result=fail submit=%d\r\n",(int)es);goto fail;
  }
  say(emit,"LFN SDR ENERGY result=ok\r\n");
 }
 memset(&spectrum,0,sizeof(spectrum));
 spectrum.session_id=session;spectrum.capture_id=1;spectrum.config_id=1;spectrum.stream_seq=5;
 spectrum.first_raw=1679;spectrum.end_raw=198287;spectrum.flags=LFN_VALID;
 spectrum.valid=true;spectrum.test_source=true;spectrum.psd[10]=0.25f;
 {
  StoragePumpTicket t;
  StorageResult ps=lfn_storage_task_psd(&spectrum,&t);
  if(ps!=STORE_OK||!wait_ticket(t,emit,"PSD")){
   say(emit,"LFN SDR PSD result=fail submit=%d\r\n",(int)ps);goto fail;
  }
  say(emit,"LFN SDR PSD result=ok\r\n");
 }
 if(!wait_synced(SD_REC_RECORDS,emit,"SYNC-RED")){say(emit,"LFN SDR SYNC records=fail\r\n");goto fail;}
 if(lfn_storage_task_health(&h))say(emit,"LFN SDR SYNC records=ok synced=%llu failed=%llu\r\n",(unsigned long long)h.pump.synced,(unsigned long long)h.pump.failed);
 if(lfn_storage_task_close()!=STORE_OK){say(emit,"LFN SDR CLOSE request=fail\r\n");goto fail;}
 for(unsigned i=0;i<SD_REC_CLOSE_MS/50;i++){
  if(lfn_storage_task_sd_closed())break;
  vTaskDelay(pdMS_TO_TICKS(50));
 }
 if(!lfn_storage_task_health(&h)||!h.closed){
  say(emit,"LFN SDR CLOSE result=fail closed=%d failed=%d sd_fault=%d\r\n",h.closed?1:0,h.close_failed?1:0,(int)h.sd_fault);
  goto fail;
 }
 say(emit,"LFN SDR CLOSE result=ok synced=%llu failed=%llu writer=%u\r\n",
     (unsigned long long)h.pump.synced,(unsigned long long)h.pump.failed,(unsigned)h.pump.writer_state);
 say(emit,"LFN SDR FILE name=%s\r\n",name);
 say(emit,"LFN SDR DONE result=ok\r\n");
 return true;
fail:
 say(emit,"LFN SDR DONE result=fail\r\n");
 return false;
}
/* Shared definition bootstrap: new file + SESSION/CONFIG/CAPTURE synced. */
static bool defs_new(uint64_t *session_out,char name[16],void (*emit)(const char *)){
 unsigned token=(unsigned)(xTaskGetTickCount()*2654435761u)&0xFFFFFu;
 (void)snprintf(name,16,"LFR%05X.LFR",token);
 uint64_t session=(((uint64_t)token)<<32)|UINT64_C(0x5A5A5A5A);
 if(!lfn_storage_task_start_sd(session,session^UINT64_C(0x9E3779B97F4A7C15),UINT64_C(12345),UINT64_C(5000000),3,name)){
  say(emit,"LFN SDL START result=fail\r\n");return false;
 }
 *session_out=session;
 StorageTaskHealth h;
 for(unsigned i=0;i<SD_REC_WAIT_MS/10;i++){
  if(!lfn_storage_task_health(&h)||h.ready||h.startup_failed)break;
  vTaskDelay(dly(10));
 }
 if(!h.ready){say(emit,"LFN SDL READY result=fail\r\n");return false;}
 cfg_session=session;cfg_config=(PlCaptureConfig){0,PL_CAPTURE_TEST,0,3,1};
 StorageRequest r;
 StorageSessionFacts sf={"LFN_AX7021",session,0,mono_ns(),hash64,true};
 if(!storage_build_session(&builder,&sf,&r,payload,sizeof(payload))||!metadata_wait(&r,emit,"SDL-SESSION"))return false;
 StorageConfigFacts cf={0};
 cf.device_id="LFN_AX7021";cf.config_hash=hash64;cf.hardware_id="AX7021_PRE_ADC";
 cf.gain_setting_id="UNVERIFIED";cf.adc_id="NONE";cf.microphone_id="NONE";
 cf.point_pose_id="NONE";cf.band_definition_id="LFN_THIRD_OCTAVE";
 cf.session_id=session;cf.calibration_id=0;cf.config=cfg_config;
 cf.event=(StorageEventFacts){6,3,60,5,20,5e-10,3,16};
 if(!storage_build_config(&builder,&cf,&r,payload,sizeof(payload))||!metadata_wait(&r,emit,"SDL-CONFIG"))return false;
 StorageCaptureFacts cap={0};
 cap.device_id="LFN_AX7021";cap.config=cfg_config;cap.max_age_ticks=100;cap.revision=1;
 cap.start_ns=mono_ns();cap.mono_ns=cap.start_ns;cap.ns_per_raw=1e9/48000.0;cap.uncertainty_ns=10000000;
 cap.started.seq=2;cap.started.flags=PL_CAPTURE_F_RUNNING|PL_CAPTURE_F_POSITION_VALID|PL_CAPTURE_F_TEST;
 cap.started.capture_id=1;cap.started.active_config_id=1;cap.started.configured=cfg_config;
 cap.active.seq=2;cap.active.kind=PL_CONTRACT_ACTIVE;cap.active.mode=cfg_config.mode;
 cap.active.source=cfg_config.source;cap.active.channel=cfg_config.channel;cap.active.pattern=cfg_config.pattern;
 cap.active.bound_mask=cfg_config.mode?4:11;
 cap.active.plan=(CapturePlan){session,1,1,false,{5,6,7,8}};
 cap.first_valid_raw=1679;cap.mono_raw=cap.first_valid_raw;
 for(unsigned k=0;k<4;k++)if(cap.active.bound_mask&(1u<<k))
  cap.active.anchors[k]=(CaptureAnchor){session,1,1,k+1,cap.active.plan.first_seq[k],cap.first_valid_raw};
 if(!storage_build_capture(&builder,&cap,&r,payload,sizeof(payload))||!metadata_wait(&r,emit,"SDL-CAPTURE"))return false;
 if(!wait_synced(3,emit,"SDL-DEF"))return false;
 return true;
}
bool lfn_sd_load_run(unsigned count,void (*emit)(const char *line)){
 if(!count||count>2000)return false;
 uint64_t session=0;char name[16];
 if(!defs_new(&session,name,emit)){say(emit,"LFN SDL DONE result=fail\r\n");return false;}
 say(emit,"LFN SDL READY name=%s session=%llu\r\n",name,(unsigned long long)session);
 for(unsigned n=0;n<count;n++){
  memset(&spectrum,0,sizeof(spectrum));
  spectrum.session_id=session;spectrum.capture_id=1;spectrum.config_id=1;
  spectrum.stream_seq=5+n;spectrum.first_raw=1679+(uint64_t)n*98304u;
  spectrum.end_raw=spectrum.first_raw+196608;spectrum.flags=LFN_VALID;
  spectrum.valid=true;spectrum.test_source=true;spectrum.psd[10]=0.25f;
  StoragePumpTicket t;
  if(lfn_storage_task_psd(&spectrum,&t)!=STORE_OK){say(emit,"LFN SDL WRITE n=%u result=busy\r\n",n);vTaskDelay(dly(10));continue;}
  if(!wait_ticket(t,emit,"SDL-PSD")){say(emit,"LFN SDL WRITE n=%u result=ticket_fail\r\n",n);return false;}
  if((n%5)==0)say(emit,"LFN SDL WRITE n=%u\r\n",n);
  vTaskDelay(dly(20));
 }
 say(emit,"LFN SDL DONE result=ok\r\n");return true;
}
bool lfn_sd_open_run(uint64_t session,const char *relative_path,void (*emit)(const char *line)){
 if(!session||!relative_path||!*relative_path)return false;
 if(!lfn_storage_task_start_sd_existing(session,session^UINT64_C(0x9E3779B97F4A7C15),UINT64_C(12345),UINT64_C(5000000),3,relative_path)){
  say(emit,"LFN SDO START result=fail\r\n");say(emit,"LFN SDO DONE result=fail\r\n");return false;
 }
 StorageTaskHealth h;memset(&h,0,sizeof(h));bool ready=false;
 for(unsigned i=0;i<SD_REC_WAIT_MS/10;i++){
  if(!lfn_storage_task_health(&h))break;
  if(h.ready){ready=true;break;}
  if(h.startup_failed)break;
  vTaskDelay(dly(10));
 }
 if(!ready){
  say(emit,"LFN SDO START result=fail startup_failed=%d sd_stage=%u sd_last_fr=%u writer=%u werr=%u tail=%d\r\n",h.startup_failed?1:0,(unsigned)h.sd_stage,h.sd_last_error,h.pre_writer_state,h.pre_writer_error,h.pre_tail_reason);
  say(emit,"LFN SDO DONE result=fail\r\n");return false;
 }
 say(emit,"LFN SDO START result=ok name=%s recovered=%llu prefix=%llu file_bytes=%llu tail_repaired=%d tail_reason=%d\r\n",
     relative_path,(unsigned long long)h.recovered_records,(unsigned long long)h.file_prefix,
     (unsigned long long)h.file_bytes,h.tail_repaired?1:0,h.tail_reason);
 if(lfn_storage_task_close()!=STORE_OK){say(emit,"LFN SDO CLOSE result=fail\r\n");say(emit,"LFN SDO DONE result=fail\r\n");return false;}
 for(unsigned i=0;i<SD_REC_CLOSE_MS/50;i++){
  if(lfn_storage_task_sd_closed())break;
  vTaskDelay(dly(50));
 }
 memset(&h,0,sizeof(h));bool got=lfn_storage_task_health(&h);
 bool ok=got&&h.closed&&h.recovered_records>0;
 say(emit,"LFN SDO CLOSE result=%s closed=%d failed=%d sd_fault=%d\r\n",ok?"ok":"fail",h.closed?1:0,h.close_failed?1:0,(int)h.sd_fault);
 say(emit,"LFN SDO RECOVER recovered=%llu prefix=%llu file_bytes=%llu tail_repaired=%d\r\n",
     (unsigned long long)h.recovered_records,(unsigned long long)h.file_prefix,
     (unsigned long long)h.file_bytes,h.tail_repaired?1:0);
 say(emit,"LFN SDO DONE result=%s\r\n",ok?"ok":"fail");
 return ok;
}
bool lfn_sd_mf_run(const char *dir,void (*emit)(const char *line)){
 if(!dir||!*dir||strlen(dir)>8)return false;
 void *self=(void*)xTaskGetCurrentTaskHandle();
 if(!lfn_sd_mkdir_probe_run(self,emit,dir)){say(emit,"LFN SDM MKDIR result=fail\r\n");return false;}
 unsigned token=(unsigned)(xTaskGetTickCount()*2654435761u)&0xFFFFFu;
 uint64_t session=(((uint64_t)token)<<32)|UINT64_C(0x5A5A5A5A);
 if(!lfn_storage_task_start_sd_multifile(session,UINT64_C(12345),UINT64_C(1000000),3,dir)){
  say(emit,"LFN SDM START result=fail\r\n");say(emit,"LFN SDM DONE result=fail\r\n");return false;
 }
 StorageTaskOwnerHealth h;bool ready=false;
 for(unsigned i=0;i<12000;i++){
  if(!lfn_storage_task_owner_health(&h))break;
  if(h.failed)break;
  if(h.phase>=8){ready=true;break;} /* cold start + 2 rotations + STOPPED */
  vTaskDelay(dly(10));
 }
 say(emit,"LFN SDM START result=%s session=%llu root=%s\r\n",ready?"ok":"fail",(unsigned long long)session,dir);
 say(emit,"LFN SDM HEALTH started=%d mode=%d ready=%d failed=%d closed=%d owner_state=%u owner_error=%u phase=%u steps=%llu sd_stage=%u last_error=%u epoch=%u life=%u rot=%u attached=%u gen=%llu sd_fault=%d\r\n",
     h.started?1:0,h.mode?1:0,h.ready?1:0,h.failed?1:0,h.closed?1:0,h.owner_state,h.owner_error,h.phase,
     (unsigned long long)h.steps,(unsigned)h.sd_stage,h.last_error,h.epoch_state,h.lifecycle_state,h.rotation_state,h.route_attached,(unsigned long long)h.route_generation,(int)h.sd_fault);
 say(emit,"LFN SDM DONE result=%s\r\n",ready?"ok":"fail");
 return ready;
}
bool lfn_sd_ls_run(const char *dir,void (*emit)(const char *line)){
 return lfn_sd_list_probe_run((void*)xTaskGetCurrentTaskHandle(),emit,dir);
}
static LfnFatFsVolume rp_volume;static LfnFatFsFile rp_file;static StorageBackend rp_backend;
static StorageFamilyReplay rp_replay;
bool lfn_sd_replay_run(uint64_t session,const char *dir,void (*emit)(const char *line)){
 /* Sealed family ledger files carry the SFL1 commit frames of each epoch. */
 static const char *files[3]={"AIDX.SFL","BIDX.SFL","CIDX.SFL"};
 if(!session||!dir||!*dir||!emit)return false;
 void *self=(void*)xTaskGetCurrentTaskHandle();
 if(!lfn_fatfs_volume_init(&rp_volume,"0:",lfn_sd_probe_owner_ok(self),NULL)){
  say(emit,"LFN SDRP VOLUME result=fail\r\n");return false;
 }
 if(lfn_fatfs_mount(&rp_volume)!=STORE_IO_OK){say(emit,"LFN SDRP MOUNT result=fail\r\n");return false;}
 uint64_t ledgers[3]={(session^UINT64_C(0x100000000000))|0x101,
                      (session^UINT64_C(0x100000000000))|0x201,
                      (session^UINT64_C(0x300000000000))|0x101};
 unsigned passed=0,failed=0,skipped=0;
 for(unsigned f=0;f<3;f++){
  char path[64];(void)snprintf(path,sizeof(path),"%s/%s",dir,files[f]);
  if(lfn_fatfs_open(&rp_file,&rp_volume,path,false,false)!=STORE_IO_OK){
   say(emit,"LFN SDRP FILE name=%s result=missing\r\n",files[f]);skipped++;continue;
  }
  bool ok=false;StorageFamilyScan state=FAMILY_SCAN_IO;unsigned count=0;uint64_t prefix=0,last=0;uint64_t used=0;
  if(lfn_fatfs_backend(&rp_file,&rp_backend)){
   for(unsigned l=0;l<3&&!ok;l++){
    if(!storage_family_replay_init(&rp_replay,&rp_backend,session,ledgers[l]))continue;
    for(unsigned k=0;k<20000;k++){
     storage_family_replay_step(&rp_replay);
     if(rp_replay.state==FAMILY_SCAN_DONE||rp_replay.state==FAMILY_SCAN_TAIL||rp_replay.state==FAMILY_SCAN_IO||rp_replay.state==FAMILY_SCAN_CAPACITY)break;
    }
    if((rp_replay.state==FAMILY_SCAN_DONE||rp_replay.state==FAMILY_SCAN_TAIL)&&rp_replay.prefix){ok=true;state=rp_replay.state;count=rp_replay.count;prefix=rp_replay.prefix;last=rp_replay.last_sequence;used=ledgers[l];break;}
   }
  }
  (void)lfn_fatfs_close(&rp_file);
  say(emit,"LFN SDRP FILE name=%s state=%u count=%u prefix=%llu last_seq=%llu ledger=%llu result=%s\r\n",
      files[f],(unsigned)state,count,(unsigned long long)prefix,(unsigned long long)last,
      (unsigned long long)used,ok?"ok":"fail");
  if(ok)passed++;else failed++;
 }
 say(emit,"LFN SDRP SUMMARY dir=%s passed=%u failed=%u skipped=%u\r\n",dir,passed,failed,skipped);
 StorageIoResult u=lfn_fatfs_unmount(&rp_volume);
 bool good=u==STORE_IO_OK&&failed==0&&passed>0;
 say(emit,"LFN SDRP DONE result=%s\r\n",good?"ok":"fail");
 return good;
}
bool lfn_sd_mw_run(const char *dir,void (*emit)(const char *line)){
 if(!dir||!*dir||strlen(dir)>8)return false;
 void *self=(void*)xTaskGetCurrentTaskHandle();
 if(!lfn_sd_mkdir_probe_run(self,emit,dir)){say(emit,"LFN SDMW MKDIR result=fail\r\n");return false;}
 unsigned token=(unsigned)(xTaskGetTickCount()*2654435761u)&0xFFFFFFu;
 uint64_t session=(((uint64_t)token)<<32)|UINT64_C(0xA5A5A5A5);
 if(!lfn_storage_task_start_sd_write(session,UINT64_C(12345),UINT64_C(1000000),3,dir,1000000)){
  say(emit,"LFN SDMW START result=fail\r\n");return false;
 }
 StorageTaskOwnerHealth h;bool running=false;
 say(emit,"LFN SDMW BEGIN dir=%s session=%llu\r\n",dir,(unsigned long long)session);
 for(unsigned i=0;i<600;i++){
  if(!lfn_storage_task_owner_health(&h)){say(emit,"LFN SDMW HEALTH read=fail\r\n");break;}
  if(h.failed){say(emit,"LFN SDMW FAIL state=%u err=%u stage=%u\r\n",h.owner_state,h.owner_error,h.sd_stage);break;}
  if(h.ready&&h.owner_state==5&&h.written>=3){running=true;break;}
  if((i%100)==99)say(emit,"LFN SDMW WAIT state=%u ready=%u failed=%u written=%llu phase=%u\r\n",h.owner_state,h.ready?1:0,h.failed?1:0,(unsigned long long)h.written,h.phase);
  vTaskDelay(dly(10));
 }
 say(emit,"LFN SDMW START result=%s session=%llu dir=%s written=%llu state=%u\r\n",
     running?"ok":"fail",(unsigned long long)session,dir,(unsigned long long)h.written,h.owner_state);
 return running;
}
bool lfn_sd_purge_dir(const char *dir,void (*emit)(const char *line)){
 return lfn_sd_purge_run((void*)xTaskGetCurrentTaskHandle(),emit,dir);
}
bool lfn_sd_purge_all(void (*emit)(const char *line)){
 return lfn_sd_purge_all_run((void*)xTaskGetCurrentTaskHandle(),emit);
}
