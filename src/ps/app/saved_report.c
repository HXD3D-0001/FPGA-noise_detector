#include "saved_report.h"
#include "../storage/marker_record.h"
#include <math.h>
#include <string.h>
static bool fail(LfnSavedReport*r,LfnSavedReportError e){r->error=e;return false;}
static bool add64(uint64_t*a,uint64_t b){if(b>UINT64_MAX-*a)return false;*a+=b;return true;}
static bool text(MetadataJson*j,const char*k,char*out,size_t n){return metadata_json_string(j,metadata_json_field(j,0,k),out,n);}
static LfnSavedReportConfig *config(LfnSavedReport*r,uint32_t id){
 for(unsigned k=0;k<r->config_count;k++)if(r->configs[k].metadata.identity.config_id==id)return &r->configs[k];
 return NULL;
}
static StorageMetadata *capture(LfnSavedReport*r,uint64_t id,uint32_t cfg,uint64_t revision){
 for(unsigned k=r->capture_count;k;k--){StorageMetadata*m=&r->captures[k-1];
  if(m->identity.capture_id==id&&m->identity.config_id==cfg&&(!revision||m->revision==revision))return m;}
 return NULL;
}
static bool bind(LfnSavedReport*r,const RecordIdentity*i){
 if(!r->have_identity){LfnSavedReportConfig*c=config(r,i->config_id);if(!c||!capture(r,i->capture_id,i->config_id,0))return fail(r,SAVED_REPORT_DEPENDENCY);
  r->capture=i->capture_id;r->config=i->config_id;r->key=c->key;r->have_identity=true;}
 if(r->capture!=i->capture_id||r->config!=i->config_id)return fail(r,SAVED_REPORT_UNSUPPORTED);
 return true;
}
static bool config_key(LfnSavedReport*r,LfnSavedReportKey*k){
 memset(k,0,sizeof(*k));StorageMetadata*m=&r->metadata;MetadataJson*j=&r->scratch;
 k->config=m->config;k->config.config_id=0;k->calibration=m->calibration_id;k->gain_verified=m->gain_verified;
 if(k->gain_verified&&!metadata_json_double(j,metadata_json_field(j,0,"analog_gain_db"),&k->analog_gain_db))return false;
 if(!metadata_json_bool(j,metadata_json_field(j,0,"fs_measured"),&k->rate_measured))return false;
 if(k->rate_measured&&!metadata_json_double(j,metadata_json_field(j,0,"measured_raw_fs_hz"),&k->measured_raw_rate))return false;
 return text(j,"hardware_id",k->hardware,sizeof k->hardware)&&text(j,"gain_setting_id",k->gain,sizeof k->gain)&&
  text(j,"point_pose_id",k->point_pose,sizeof k->point_pose)&&text(j,"adc_id",k->adc,sizeof k->adc)&&text(j,"microphone_id",k->microphone,sizeof k->microphone)&&
  text(j,"algorithm_hash",k->algorithm,sizeof k->algorithm)&&text(j,"rom_manifest_hash",k->rom,sizeof k->rom)&&text(j,"window_hash",k->window,sizeof k->window)&&text(j,"band_definition_id",k->bands,sizeof k->bands);
}
void lfn_saved_report_init(LfnSavedReport*r){if(r)memset(r,0,sizeof(*r));}
bool lfn_saved_report_add(LfnSavedReport*r,const RecordView*v){
 if(!r||!v||r->error)return false;
 const RecordIdentity*i=&v->identity;
 if(!i->session_id||!i->record_seq||i->record_seq<=r->last_sequence||!v->payload)return fail(r,SAVED_REPORT_INVALID);
 if(r->have_session&&r->session!=i->session_id)return fail(r,SAVED_REPORT_INVALID);
 r->last_sequence=i->record_seq;
 if(i->type==RECORD_SESSION||i->type==RECORD_CONFIG||i->type==RECORD_CAPTURE){
  if(!storage_metadata_decode(i,v->payload,v->payload_bytes,&r->scratch,&r->metadata))return fail(r,SAVED_REPORT_INVALID);
  StorageMetadata*m=&r->metadata;
  if(i->type==RECORD_SESSION){if(r->have_session)return fail(r,SAVED_REPORT_INVALID);r->have_session=true;r->session=i->session_id;memcpy(r->device,m->device_id,sizeof r->device);return true;}
  if(!r->have_session||strcmp(r->device,m->device_id))return fail(r,SAVED_REPORT_DEPENDENCY);
  if(i->type==RECORD_CONFIG){
   if(config(r,i->config_id))return fail(r,SAVED_REPORT_INVALID);
   if(r->config_count==SAVED_REPORT_CONFIGS)return fail(r,SAVED_REPORT_CAPACITY);
   LfnSavedReportConfig*c=&r->configs[r->config_count];c->metadata=*m;if(!config_key(r,&c->key))return fail(r,SAVED_REPORT_INVALID);r->config_count++;return true;
  }
  if(!config(r,i->config_id))return fail(r,SAVED_REPORT_DEPENDENCY);
  if(r->capture_count==SAVED_REPORT_CAPTURES)return fail(r,SAVED_REPORT_CAPACITY);
  r->captures[r->capture_count++]=*m;return true;
 }
 if(!r->have_session)return fail(r,SAVED_REPORT_DEPENDENCY);
 if(i->type==RECORD_MARKER){
  if(!storage_marker_decode(i,v->payload,v->payload_bytes,&r->scratch,&r->marker))return fail(r,SAVED_REPORT_INVALID);
  char device[64];if(!text(&r->scratch,"device_id",device,sizeof device)||strcmp(device,r->device)||!bind(r,i))return fail(r,r->error?r->error:SAVED_REPORT_DEPENDENCY);
  StorageMetadata*c=capture(r,i->capture_id,i->config_id,0);
  if(r->marker.start.raw_index<c->origin_raw||(c->closed&&r->marker.end.raw_index>c->identity.end_raw))return fail(r,SAVED_REPORT_INVALID);
  for(unsigned k=0;k<r->marker_count;k++)if(r->markers[k].marker_id==r->marker.marker_id)return fail(r,SAVED_REPORT_INVALID);
  if(r->marker_count==LFN_MARKER_CAPACITY)return fail(r,SAVED_REPORT_CAPACITY);
  r->markers[r->marker_count++]=r->marker;return true;
 }
 if(i->type==RECORD_MINUTE_SUMMARY){
  if(!storage_summary_decode(i,v->payload,v->payload_bytes,&r->scratch,&r->summary))return fail(r,SAVED_REPORT_INVALID);
  MinuteSummary*m=&r->summary;
  if(m->schema_version!=2||m->revision!=1)return fail(r,SAVED_REPORT_UNSUPPORTED);
  if(!bind(r,i))return false;
  StorageMetadata*c=capture(r,i->capture_id,i->config_id,m->capture_revision);
  if(!c||strcmp(m->identity.device_id,r->device)||strcmp(m->identity.band_definition_id,r->key.bands)||m->first_raw<c->origin_raw||
   (c->closed&&m->end_raw>c->identity.end_raw))return fail(r,SAVED_REPORT_DEPENDENCY);
  if(c->time_quality!=STORAGE_TIME_NOMINAL_PAIRED||m->time_pair.anchor_raw!=c->time_pair.anchor_raw||m->time_pair.anchor_ns!=c->time_pair.anchor_ns||m->time_pair.uncertainty_ns!=c->time_pair.uncertainty_ns||m->time_pair.a_seq!=c->time_pair.a_seq||m->time_pair.b_seq!=c->time_pair.b_seq)return fail(r,SAVED_REPORT_DEPENDENCY);
  if(r->have_range&&(m->first_raw<r->last_end||m->summary_id<=r->last_summary_id))return fail(r,SAVED_REPORT_INVALID);
  if(!add64(&r->duration_ns,m->requested_ns)||!add64(&r->valid_ns,m->valid_ns)||!add64(&r->expected_candidates,m->expected_candidates)||!add64(&r->valid_candidates,m->valid_candidates))return fail(r,SAVED_REPORT_INVALID);
  for(unsigned k=0;k<SUMMARY_BANDS;k++){r->weighted[k]+=m->sum_power_weighted[k];if(!isfinite(r->weighted[k]))return fail(r,SAVED_REPORT_INVALID);}
  r->last_end=m->end_raw;r->last_summary_id=m->summary_id;r->have_range=true;r->summary_count++;return true;
 }
 /* ASUM may also hold fault/time/calibration definitions; they do not become
  * levels or fabricated clipped-sample counts. No acoustic calibration applied. */
 return true;
}
bool lfn_saved_report_finish(LfnSavedReport*r,LfnReportData*d){
 if(!r||!d||r->error||!r->have_session||!r->have_identity)return false;
 memset(d,0,sizeof(*d));d->session_id=r->session;d->capture_id=r->capture;d->config_id=r->config;d->persisted=true;
 d->source=r->key.config.source==PL_CAPTURE_ADC?LFN_REPORT_SOURCE_ADC:r->key.config.source==PL_CAPTURE_TEST?LFN_REPORT_SOURCE_TEST:LFN_REPORT_SOURCE_UNKNOWN;
 d->adc_left=r->key.config.channel==0;
 d->duration_ns=d->expected_ns=r->duration_ns;d->valid_ns=r->valid_ns;
 d->markers=r->markers;d->marker_count=r->marker_count;d->start_uncertainty_ns=UINT64_MAX;
 d->dropped_frames=r->expected_candidates-r->valid_candidates;
 memset(&r->levels,0,sizeof r->levels);r->levels.duration_ns=r->duration_ns;r->levels.expected_ns=r->duration_ns;r->levels.valid_ns=r->valid_ns;r->levels.dropped_frames=d->dropped_frames;
 if(r->summary_count){r->levels.band_count=SUMMARY_BANDS;for(unsigned k=0;k<SUMMARY_BANDS;k++){
  double center=1000*pow(10.0,((double)k-15)/10);LfnBandLevel*b=&r->levels.bands[k];b->low_hz=center/pow(10.0,.05);b->high_hz=center*pow(10.0,.05);
  b->power_linear=r->valid_ns?r->weighted[k]/((double)r->valid_ns/1e9):0;b->below_floor=!r->valid_ns||b->power_linear<=0;
 }d->live=&r->levels;}
 return true;
}
bool lfn_saved_report_compare(LfnSavedReport*a,LfnSavedReport*b,LfnCompareResult*out){
 if(!a||!b||!out)return false;
 LfnReportData da,db;if(!lfn_saved_report_finish(a,&da)||!lfn_saved_report_finish(b,&db)||!a->summary_count||!b->summary_count){memset(out,0,sizeof(*out));out->status=LFN_COMPARE_INVALID;return false;}
 const LfnSavedReportKey*x=&a->key,*y=&b->key;unsigned mismatch=0;
 if(strcmp(x->point_pose,y->point_pose))mismatch|=LFN_COMPARE_MISMATCH_POINT|LFN_COMPARE_MISMATCH_POSE;
 if(strcmp(x->gain,y->gain)||x->gain_verified!=y->gain_verified||x->analog_gain_db!=y->analog_gain_db)mismatch|=LFN_COMPARE_MISMATCH_GAIN;
 if(strcmp(x->hardware,y->hardware)||strcmp(x->adc,y->adc)||strcmp(x->microphone,y->microphone)||memcmp(&x->config,&y->config,sizeof x->config)||x->rate_measured!=y->rate_measured||x->measured_raw_rate!=y->measured_raw_rate)mismatch|=LFN_COMPARE_MISMATCH_CONFIG;
 if(strcmp(x->algorithm,y->algorithm)||strcmp(x->rom,y->rom)||strcmp(x->window,y->window))mismatch|=LFN_COMPARE_MISMATCH_ALGORITHM;
 if(strcmp(x->bands,y->bands))mismatch|=LFN_COMPARE_MISMATCH_BANDS;
 if(x->calibration!=y->calibration)mismatch|=LFN_COMPARE_MISMATCH_CALIBRATION;
 /* Matching declared settings support relative full-scale comparison. Unknown
  * physical conditions remain explicitly unconfirmed in the stored report. */
 if(mismatch){memset(out,0,sizeof(*out));out->status=LFN_COMPARE_INCOMPATIBLE;out->mismatch_mask=mismatch;return true;}
 return lfn_level_compare(&a->levels,&b->levels,out);
}
