#include "metadata_builder.h"
#include "storage_profile.h"
#include "../app/capture_time_contract.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
static bool identifier(const char*s){if(!s||!*s)return false;for(unsigned k=0;k<64;k++){unsigned char c=(unsigned char)s[k];if(!c)return true;if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.'))return false;}return false;}
static bool hash(const char*s){if(!s)return false;for(unsigned k=0;k<64;k++)if(!((s[k]>='0'&&s[k]<='9')||(s[k]>='a'&&s[k]<='f')))return false;return !s[64];}
static void begin(StorageMetadataBuilder*b){b->used=0;b->ok=true;}
static void add(StorageMetadataBuilder*b,const char*fmt,...){if(!b->ok)return;va_list args;va_start(args,fmt);int n=vsnprintf((char*)b->bytes+b->used,sizeof(b->bytes)-b->used,fmt,args);va_end(args);if(n<0||(size_t)n>=sizeof(b->bytes)-b->used){b->ok=false;return;}b->used+=(size_t)n;}
static void number(StorageMetadataBuilder*b,uint64_t n){add(b,"\"%llu\"",(unsigned long long)n);}
static void common_version(StorageMetadataBuilder*b,const char*device,uint64_t session,uint64_t capture,uint32_t config,unsigned version){
 add(b,"{\"schema_version\":%u,\"device_id\":\"%s\",\"session_id\":",version,device);number(b,session);add(b,",\"capture_id\":");if(capture)number(b,capture);else add(b,"null");add(b,",\"config_id\":");if(config)add(b,"%u",(unsigned)config);else add(b,"null");
}
static void common(StorageMetadataBuilder*b,const char*d,uint64_t s,uint64_t c,uint32_t k){common_version(b,d,s,c,k,1);}
static bool finish(StorageMetadataBuilder*b,RecordIdentity id,StorageRequest*r,uint8_t*out,size_t cap){return b->ok&&out!=b->bytes&&storage_metadata_record(&id,b->bytes,b->used,&b->scratch,r,out,cap);}
bool storage_build_session(StorageMetadataBuilder*b,const StorageSessionFacts*f,StorageRequest*r,uint8_t*out,size_t cap){
 if(!b||!f||!identifier(f->device_id)||!hash(f->build_hash))return false;
 begin(b);common(b,f->device_id,f->session_id,0,0);add(b,",\"temporary\":%s,\"boot_generation\":",f->temporary?"true":"false");number(b,f->boot_generation);add(b,",\"build_manifest_hash\":\"%s\",\"start_monotonic_ns\":",f->build_hash);number(b,f->start_ns);add(b,"}");
 return finish(b,(RecordIdentity){RECORD_SESSION,0,0,f->session_id,0,0,0,0},r,out,cap);
}
bool storage_build_config(StorageMetadataBuilder*b,const StorageConfigFacts*f,StorageRequest*r,uint8_t*out,size_t cap){
 if(!b||!f||!identifier(f->device_id)||!hash(f->config_hash)||!identifier(f->hardware_id)||!identifier(f->gain_setting_id)||!identifier(f->adc_id)||!identifier(f->microphone_id)||!identifier(f->point_pose_id)||!identifier(f->band_definition_id))return false;
 begin(b);common(b,f->device_id,f->session_id,0,f->config.config_id);
 add(b,",\"config_hash\":\"%s\",\"hardware_id\":\"%s\",\"gain_setting_id\":\"%s\",\"adc_id\":\"%s\",\"microphone_id\":\"%s\",\"point_pose_id\":\"%s\",\"band_definition_id\":\"%s\",\"mode\":%u,\"source\":%u,\"channel\":%u,\"pattern\":%u,\"gain_status\":\"%s\",\"analog_gain_db\":",f->config_hash,f->hardware_id,f->gain_setting_id,f->adc_id,f->microphone_id,f->point_pose_id,f->band_definition_id,(unsigned)f->config.mode,(unsigned)f->config.source,(unsigned)f->config.channel,(unsigned)f->config.pattern,f->gain_verified?"VERIFIED":"UNVERIFIED");
 if(f->gain_verified)add(b,"%.17g",f->gain_db);else add(b,"null");
 add(b,",\"fs_measured\":%s,\"measured_raw_fs_hz\":",f->fs_measured?"true":"false");if(f->fs_measured)add(b,"%.17g",f->measured_raw_fs_hz);else add(b,"null");add(b,",\"fs_uncertainty_ppm\":");if(f->fs_measured)add(b,"%.17g",f->fs_uncertainty_ppm);else add(b,"null");
 add(b,",\"calibration_id\":");if(f->calibration_id)number(b,f->calibration_id);else add(b,"null");
 const StorageEventFacts*e=&f->event;add(b,",\"event_thresholds\":{\"high_db\":%.17g,\"low_db\":%.17g,\"background_seconds\":%.17g,\"min_active_seconds\":%.17g,\"tail_seconds\":%.17g,\"power_floor\":%.17g,\"high_updates\":",e->high_db,e->low_db,e->background_seconds,e->min_active_seconds,e->tail_seconds,e->power_floor);number(b,e->high_updates);add(b,",\"low_updates\":");number(b,e->low_updates);add(b,"},%s}",lfn_storage_profile);
 return finish(b,(RecordIdentity){RECORD_CONFIG,0,f->config.config_id,f->session_id,0,0,0,0},r,out,cap);
}
static bool same_config(const PlCaptureConfig*a,const PlCaptureConfig*b){return a->config_id==b->config_id&&a->mode==b->mode&&a->source==b->source&&a->channel==b->channel&&a->pattern==b->pattern;}
bool storage_build_capture(StorageMetadataBuilder*b,const StorageCaptureFacts*f,StorageRequest*r,uint8_t*out,size_t cap){
 if(!b||!f||!identifier(f->device_id))return false;
 if(f->schema_version>2)return false;
 StorageCaptureFacts projected;
 if(f->schema_version==2){
  const CaptureTimePair *p=&f->time_pair;
  if(!capture_time_nominal_valid(p)||p->session_id!=f->active.plan.session_id||p->capture_id!=f->active.plan.capture_id||p->config_id!=f->config.config_id||p->source!=f->config.source)return false;
  projected=*f;
  if(!capture_time_pair_map_ns(p,f->first_valid_raw,&projected.start_ns)||(f->closed&&!capture_time_pair_map_ns(p,f->end_raw,&projected.end_ns)))return false;
  projected.mono_raw=p->anchor_raw;projected.mono_ns=p->anchor_ns;projected.uncertainty_ns=p->uncertainty_ns;projected.ns_per_raw=1e9/48000.0;f=&projected;
 }
 const PlContractSnapshot*a=&f->active;const PlCaptureSnapshot*s=&f->started;
 unsigned mask=f->config.mode?CAPTURE_RAW_MASK:CAPTURE_MONITOR_MASK;
 if(!a->seq||a->kind!=PL_CONTRACT_ACTIVE||a->age_ticks>f->max_age_ticks||!a->plan.session_id||!a->plan.capture_id||a->plan.config_id!=f->config.config_id||a->plan.raw_mode!=(f->config.mode!=0)||a->mode!=f->config.mode||a->source!=f->config.source||a->channel!=f->config.channel||a->pattern!=f->config.pattern||a->bound_mask!=mask||!s->seq||s->age_ticks>f->max_age_ticks||s->capture_id!=a->plan.capture_id||s->active_config_id!=f->config.config_id||!same_config(&s->configured,&f->config)||(s->flags&(PL_CAPTURE_F_RUNNING|PL_CAPTURE_F_POSITION_VALID))!=(PL_CAPTURE_F_RUNNING|PL_CAPTURE_F_POSITION_VALID)||!!(s->flags&PL_CAPTURE_F_TEST)!=(f->config.source==PL_CAPTURE_TEST))return false;
 for(unsigned k=0;k<4;k++)if(mask&(1u<<k)){const CaptureAnchor*x=&a->anchors[k];if(x->session_id!=a->plan.session_id||x->capture_id!=a->plan.capture_id||x->config_id!=f->config.config_id||x->stream!=k+1||x->first_seq!=a->plan.first_seq[k])return false;}
 if(f->schema_version==2&&f->first_valid_raw!=a->anchors[f->config.mode?2:3].first_raw)return false;
 if(f->closed){const PlCaptureSnapshot*t=&f->stopped;if(!f->software_drained||!identifier(f->end_reason)||!t->seq||t->age_ticks>f->max_age_ticks||t->capture_id!=s->capture_id||t->active_config_id!=s->active_config_id||t->origin_raw!=s->origin_raw||(t->flags&(PL_CAPTURE_F_STOP_COMPLETE|PL_CAPTURE_F_POSITION_VALID))!=(PL_CAPTURE_F_STOP_COMPLETE|PL_CAPTURE_F_POSITION_VALID)||(t->flags&PL_CAPTURE_F_RUNNING)||t->latest_raw==UINT64_MAX||f->end_raw!=t->latest_raw+1)return false;}
 begin(b);common_version(b,f->device_id,a->plan.session_id,a->plan.capture_id,f->config.config_id,f->schema_version==2?2:1);add(b,",\"capture_revision\":");number(b,f->revision);add(b,",\"state\":\"%s\",\"mode\":%u,\"source\":%u,\"channel\":%u,\"pattern\":%u,\"origin_raw\":",f->closed?"CLOSED":"OPEN",(unsigned)f->config.mode,(unsigned)f->config.source,(unsigned)f->config.channel,(unsigned)f->config.pattern);number(b,s->origin_raw);add(b,",\"first_valid_raw\":");number(b,f->first_valid_raw);add(b,",\"bound_mask\":%u,\"first_stream_seq\":[",mask);
 for(unsigned k=0;k<4;k++){if(k)add(b,",");if(mask&(1u<<k))number(b,a->anchors[k].first_seq);else add(b,"null");}add(b,"],\"first_candidate_raw\":[");for(unsigned k=0;k<4;k++){if(k)add(b,",");if(mask&(1u<<k))number(b,a->anchors[k].first_raw);else add(b,"null");}
 add(b,"],\"start_monotonic_ns\":");number(b,f->start_ns);add(b,",\"mono_anchor_raw\":");number(b,f->mono_raw);add(b,",\"mono_anchor_ns\":");number(b,f->mono_ns);add(b,",\"mono_ns_per_raw\":%.17g,\"mono_uncertainty_ns\":",f->ns_per_raw);number(b,f->uncertainty_ns);add(b,",\"end_raw\":");if(f->closed)number(b,f->end_raw);else add(b,"null");add(b,",\"end_monotonic_ns\":");if(f->closed)number(b,f->end_ns);else add(b,"null");add(b,",\"end_reason\":");if(f->closed)add(b,"\"%s\"",f->end_reason);else add(b,"null");add(b,"}");
 if(f->schema_version==2){
  b->used--;const CaptureTimePair *p=&f->time_pair;
  add(b,",\"time_quality\":\"NOMINAL_PAIRED\",\"pair_method\":\"%s\",\"pair_cdc_guard_samples\":%u,\"start_time_status\":\"NOMINAL_PROJECTION\",\"nominal_ns_numerator\":\"1000000000\",\"nominal_raw_denominator\":\"48000\",\"frequency_error_ppm\":null,\"pair_before_us\":",capture_time_pair_method(p),p->source==PL_CAPTURE_ADC?1u:0u);number(b,p->before_us);add(b,",\"pair_after_us\":");number(b,p->after_us);add(b,",\"pair_lower_ns\":");number(b,p->before_us*1000);add(b,",\"pair_upper_exclusive_ns\":");number(b,(p->after_us+1)*1000);add(b,",\"pair_a_seq\":%u,\"pair_b_seq\":%u}",p->a_seq,p->b_seq);
 }
 return finish(b,(RecordIdentity){RECORD_CAPTURE,0,f->config.config_id,a->plan.session_id,a->plan.capture_id,s->origin_raw,f->closed?f->end_raw:s->origin_raw,0},r,out,cap);
}
bool storage_build_time(StorageMetadataBuilder*b,const StorageTimeFacts*f,StorageRequest*r,uint8_t*out,size_t cap){
 if(!b||!f||!identifier(f->device_id)||!identifier(f->clock_source))return false;
 begin(b);common(b,f->device_id,f->session_id,f->capture_id,f->config_id);add(b,",\"map_id\":");number(b,f->map_id);add(b,",\"anchor_raw\":");number(b,f->anchor_raw);add(b,",\"valid_from_raw\":");number(b,f->anchor_raw);add(b,",\"slope_ns_per_raw\":%.17g,\"uncertainty_ns\":",f->ns_per_raw);number(b,f->uncertainty_ns);add(b,",\"clock_source\":\"%s\",\"synchronized\":%s,\"utc_ns\":",f->clock_source,f->synchronized?"true":"false");if(f->synchronized)add(b,"\"%lld\"",(long long)f->utc_ns);else add(b,"null");add(b,"}");
 return finish(b,(RecordIdentity){RECORD_TIME_MAP,0,f->config_id,f->session_id,f->capture_id,f->anchor_raw,f->anchor_raw,0},r,out,cap);
}
