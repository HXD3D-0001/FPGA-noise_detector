#include "storage_metadata.h"
#include "marker_record.h"
#include "event_meta.h"
#include <stdarg.h>
#include "../../generated/fir_coefficients.h"
#include "record_manifest.h"
#include "../app/capture_time_contract.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdarg.h>
#define FIELD(d,o,k) metadata_json_field((d),(o),(k))
static bool null(const MetadataJson*d,int i){return i>=0&&d->token[i].type==MJ_LITERAL&&metadata_json_is(d,i,"null");}
static bool text(const MetadataJson*d,int i,const char*s){return i>=0&&d->token[i].type==MJ_STRING&&metadata_json_is(d,i,s);}
static bool identifier(const MetadataJson*d,int i,char*out,size_t cap){
 if(!metadata_json_string(d,i,out,cap)||!*out)return false;
 for(size_t k=0;out[k];k++){unsigned char c=(unsigned char)out[k];if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.'))return false;}return true;
}
static bool idfield(const MetadataJson*d,int o,const char*k){char out[64];return identifier(d,FIELD(d,o,k),out,sizeof(out));}
static bool hash(const MetadataJson*d,int i){char b[65];if(!metadata_json_string(d,i,b,sizeof(b))||strlen(b)!=64)return false;
 for(unsigned k=0;k<64;k++)if(!((b[k]>='0'&&b[k]<='9')||(b[k]>='a'&&b[k]<='f')))return false;
 return true;}
static bool u64(const MetadataJson*d,int o,const char*k,uint64_t*out){return metadata_json_u64(d,FIELD(d,o,k),out);}
static bool u32(const MetadataJson*d,int o,const char*k,uint32_t*out){return metadata_json_u32(d,FIELD(d,o,k),out);}
static bool num(const MetadataJson*d,int o,const char*k,double*out){return metadata_json_double(d,FIELD(d,o,k),out);}
static bool exact(const MetadataJson*d,int o,const char*k,uint32_t n){uint32_t v;return u32(d,o,k,&v)&&v==n;}
static bool positive(const MetadataJson*d,int o,const char*k){double n;return num(d,o,k,&n)&&n>0;}
static bool equal_number(const MetadataJson*d,int o,const char*k,double expected){double n;return num(d,o,k,&n)&&n==expected;}
static bool array(const MetadataJson*d,int i,unsigned n){return i>=0&&d->token[i].type==MJ_ARRAY&&d->token[i].count==n;}
static bool pair_method(const MetadataJson*d,uint32_t source){
 if(!text(d,FIELD(d,0,"pair_method"),source==PL_CAPTURE_ADC?"ADC_COUNTER_CROSSING":"TEST_COUNTER_CROSSING"))return false;
 return source!=PL_CAPTURE_ADC||exact(d,0,"pair_cdc_guard_samples",1);
}
static bool config_fields(const MetadataJson*d,int o,PlCaptureConfig*c){return u32(d,o,"mode",&c->mode)&&c->mode<3&&u32(d,o,"source",&c->source)&&c->source<2&&u32(d,o,"channel",&c->channel)&&c->channel<2&&u32(d,o,"pattern",&c->pattern)&&c->pattern<4;}
static bool common(const MetadataJson*d,const RecordIdentity*i,StorageMetadata*m){
 uint64_t session,capture=0;uint32_t config=0;
 uint32_t version;
 if(i->flags||!u32(d,0,"schema_version",&version)||(version!=1&&(version!=2||i->type!=RECORD_CAPTURE))||!identifier(d,FIELD(d,0,"device_id"),m->device_id,sizeof(m->device_id))||!u64(d,0,"session_id",&session)||!session||session!=i->session_id)return false;
 m->schema_version=version;
 int c=FIELD(d,0,"capture_id"),k=FIELD(d,0,"config_id");
 if(!null(d,c)&&(!metadata_json_u64(d,c,&capture)||!capture))return false;
 if(!null(d,k)&&(!metadata_json_u32(d,k,&config)||!config))return false;
 return capture==i->capture_id&&config==i->config_id;
}
static bool config(const MetadataJson*d,StorageMetadata*m){
 const RecordIdentity*i=&m->identity;uint64_t v;bool measured;double n;
 if(!i->config_id||i->capture_id||i->first_raw||i->end_raw||!config_fields(d,0,&m->config))return false;
 m->config.config_id=i->config_id;
 int histogram=FIELD(d,0,"summary_histogram");
 if(histogram>=0){
  if(!equal_number(d,histogram,"reference_power_fs2",0.5)||!equal_number(d,histogram,"min_dbfs",-120)||!exact(d,histogram,"step_db",5)||!exact(d,histogram,"bin_count",26))return false;
  m->summary_histogram=true;
 }
 const char*hashes[]={"config_hash","algorithm_hash","rom_manifest_hash","window_hash"};for(unsigned k=0;k<4;k++)if(!hash(d,FIELD(d,0,hashes[k])))return false;
 if(!text(d,FIELD(d,0,"window_hash"),LFN_METADATA_HANN_SHA)||!text(d,FIELD(d,0,"algorithm_hash"),LFN_METADATA_ALGORITHM_SHA)||!text(d,FIELD(d,0,"rom_manifest_hash"),LFN_METADATA_ROM_SHA))return false;
 if(!identifier(d,FIELD(d,0,"hardware_id"),m->hardware_id,sizeof(m->hardware_id))||!identifier(d,FIELD(d,0,"gain_setting_id"),m->gain_setting_id,sizeof(m->gain_setting_id))||!idfield(d,0,"adc_id")||!idfield(d,0,"microphone_id")||!idfield(d,0,"point_pose_id"))return false;
 if(text(d,FIELD(d,0,"gain_status"),"UNVERIFIED")){if(!null(d,FIELD(d,0,"analog_gain_db")))return false;}
 else{if(!text(d,FIELD(d,0,"gain_status"),"VERIFIED")||!num(d,0,"analog_gain_db",&n))return false;
 m->gain_verified=true;}
 if(!exact(d,0,"raw_fs_hz",48000)||!exact(d,0,"processed_fs_hz",1000)||!metadata_json_bool(d,FIELD(d,0,"fs_measured"),&measured))return false;
 if(measured){if(!positive(d,0,"measured_raw_fs_hz")||!num(d,0,"fs_uncertainty_ppm",&n)||n<0)return false;}
 else if(!null(d,FIELD(d,0,"measured_raw_fs_hz"))||!null(d,FIELD(d,0,"fs_uncertainty_ppm")))return false;
 if(!exact(d,0,"group_delay_raw_ticks",831)||!exact(d,0,"fft_count",4096)||!exact(d,0,"fft_hop",2048)||!exact(d,0,"psd_bins",1311)||!text(d,FIELD(d,0,"psd_unit"),"FS2_PER_HZ")||!text(d,FIELD(d,0,"window_definition"),"PERIODIC_HANN_Q1_17")||!positive(d,0,"window_sum_squares"))return false;
 if(!num(d,0,"window_sum_squares",&n)||fabs(n-LFN_METADATA_WINDOW_SUM)>1e-9)return false;
 int coeff=FIELD(d,0,"coefficients");if(coeff<0||!text(d,FIELD(d,coeff,"format"),"SIGNED_Q1_17")||!text(d,FIELD(d,coeff,"rounding"),"NEAREST_TIES_AWAY")||!exact(d,coeff,"accumulator_bits",56))return false;
 int stages=FIELD(d,coeff,"stages");if(!array(d,stages,3))return false;
 const int32_t*values[]={lfn_fir_stage1,lfn_fir_stage2,lfn_fir_stage3};const unsigned taps[]={67,59,53},decim[]={6,4,2};
 for(unsigned s=0;s<3;s++){
  int stage=metadata_json_index(d,stages,s);if(!exact(d,stage,"decimation",decim[s])||!exact(d,stage,"phase",decim[s]-1)||!exact(d,stage,"taps",taps[s])||!text(d,FIELD(d,stage,"sha256"),lfn_metadata_stage_sha[s]))return false;
  int a=FIELD(d,stage,"values");if(!array(d,a,taps[s]))return false;
  for(unsigned t=0;t<taps[s];t++){double x;if(!metadata_json_double(d,metadata_json_index(d,a,t),&x)||x!=values[s][t])return false;}
 }
 int bands=FIELD(d,0,"bands");if(!array(d,bands,10)||!idfield(d,0,"band_definition_id"))return false;
 for(unsigned b=0;b<10;b++){int band=metadata_json_index(d,bands,b);double low,high;if(!array(d,band,2)||!metadata_json_double(d,metadata_json_index(d,band,0),&low)||!metadata_json_double(d,metadata_json_index(d,band,1),&high)||low<0||low>=high||high>500)return false;
 double center=1000*pow(10.0,((double)b-15)/10);if(fabs(low-center/pow(10.0,0.05))>1e-9||fabs(high-center*pow(10.0,0.05))>1e-9)return false;}
 double low,high;if(!num(d,0,"target_low_hz",&low)||!num(d,0,"target_high_hz",&high)||low!=31.5||high!=250)return false;
 int e=FIELD(d,0,"event_thresholds");double hi,lo;
 if(e<0||!num(d,e,"high_db",&hi)||!num(d,e,"low_db",&lo)||lo<0||hi<=lo||!positive(d,e,"background_seconds")||!positive(d,e,"min_active_seconds")||!positive(d,e,"tail_seconds")||!u64(d,e,"high_updates",&v)||!v||!u64(d,e,"low_updates",&v)||!v||!positive(d,e,"power_floor"))return false;
 int cal=FIELD(d,0,"calibration_id");if(!null(d,cal)&&(!metadata_json_u64(d,cal,&m->calibration_id)||!m->calibration_id))return false;
 return true;
}
static bool capture(const MetadataJson*d,StorageMetadata*m){
 const RecordIdentity*i=&m->identity;uint32_t mask;double slope;
 if(!i->capture_id||!i->config_id||!config_fields(d,0,&m->config)||!u64(d,0,"capture_revision",&m->revision)||!m->revision||!u64(d,0,"origin_raw",&m->origin_raw)||!u64(d,0,"first_valid_raw",&m->first_valid_raw)||m->first_valid_raw<m->origin_raw||i->first_raw!=m->origin_raw)return false;
 m->config.config_id=i->config_id;unsigned required=m->config.mode?4:11;
 if(!u32(d,0,"bound_mask",&mask)||mask!=required)return false;
 m->mask=(uint8_t)mask;
 int seq=FIELD(d,0,"first_stream_seq"),raw=FIELD(d,0,"first_candidate_raw");if(!array(d,seq,4)||!array(d,raw,4))return false;
 for(unsigned k=0;k<4;k++){
  int a=metadata_json_index(d,seq,k),b=metadata_json_index(d,raw,k);
  if(required&(1u<<k)){if(!metadata_json_u64(d,a,&m->first_seq[k])||m->first_seq[k]==UINT64_MAX||!metadata_json_u64(d,b,&m->first_raw[k])||m->first_raw[k]<m->first_valid_raw||m->first_raw[k]>UINT64_MAX-196608)return false;}
  else if(!null(d,a)||!null(d,b))return false;
 }
 if(!u64(d,0,"start_monotonic_ns",&m->start_ns)||!u64(d,0,"mono_anchor_raw",&m->mono_raw)||m->mono_raw<m->origin_raw||!u64(d,0,"mono_anchor_ns",&m->mono_ns)||!num(d,0,"mono_ns_per_raw",&slope)||slope<=0||!u64(d,0,"mono_uncertainty_ns",&m->uncertainty))return false;
 m->slope=slope;
 if(m->schema_version==2){
  CaptureTimePair *p=&m->time_pair;uint64_t numerator,denominator,low,high,start;
  if(m->first_valid_raw!=m->first_raw[m->config.mode?2:3])return false;
  p->status=CAPTURE_TIME_CONFIRMED_NOMINAL;p->session_id=i->session_id;p->capture_id=i->capture_id;p->config_id=i->config_id;p->source=m->config.source;p->nominal_rate_hz=48000;p->anchor_raw=m->mono_raw;p->anchor_ns=m->mono_ns;p->uncertainty_ns=m->uncertainty;
  if(!text(d,FIELD(d,0,"time_quality"),"NOMINAL_PAIRED")||!pair_method(d,p->source)||!text(d,FIELD(d,0,"start_time_status"),"NOMINAL_PROJECTION")||!null(d,FIELD(d,0,"frequency_error_ppm"))||!u64(d,0,"nominal_ns_numerator",&numerator)||numerator!=1000000000||!u64(d,0,"nominal_raw_denominator",&denominator)||denominator!=48000||slope!=1e9/48000.0||!u64(d,0,"pair_before_us",&p->before_us)||!u64(d,0,"pair_after_us",&p->after_us)||!u32(d,0,"pair_a_seq",&p->a_seq)||!u32(d,0,"pair_b_seq",&p->b_seq)||!u64(d,0,"pair_lower_ns",&low)||!u64(d,0,"pair_upper_exclusive_ns",&high)||!capture_time_nominal_valid(p)||low!=p->before_us*1000||high!=(p->after_us+1)*1000||!capture_time_pair_map_ns(p,m->first_valid_raw,&start)||start!=m->start_ns)return false;
  m->time_quality=STORAGE_TIME_NOMINAL_PAIRED;
 }
 if(text(d,FIELD(d,0,"state"),"OPEN")){return null(d,FIELD(d,0,"end_raw"))&&null(d,FIELD(d,0,"end_monotonic_ns"))&&null(d,FIELD(d,0,"end_reason"))&&i->end_raw==m->origin_raw;}
 if(!text(d,FIELD(d,0,"state"),"CLOSED")||!u64(d,0,"end_raw",&m->object_id)||m->object_id<m->origin_raw||i->end_raw!=m->object_id||!u64(d,0,"end_monotonic_ns",&m->end_ns)||m->end_ns<m->start_ns||!idfield(d,0,"end_reason"))return false;
 if(m->schema_version==2){uint64_t end;if(!capture_time_pair_map_ns(&m->time_pair,m->object_id,&end)||end!=m->end_ns)return false;}
 m->closed=true;return true;
}
static bool calibration(const MetadataJson*d,StorageMetadata*m){
 if(!m->identity.config_id||m->identity.capture_id||m->identity.first_raw||m->identity.end_raw||!u64(d,0,"calibration_id",&m->object_id)||!m->object_id||!identifier(d,FIELD(d,0,"hardware_id"),m->hardware_id,sizeof(m->hardware_id))||!identifier(d,FIELD(d,0,"gain_setting_id"),m->gain_setting_id,sizeof(m->gain_setting_id)))return false;
 if(text(d,FIELD(d,0,"status"),"INVALID"))return idfield(d,0,"invalid_reason")&&null(d,FIELD(d,0,"pa_per_fs"));
 double low,high,n;uint64_t map;
 if(!text(d,FIELD(d,0,"status"),"VALID")||!idfield(d,0,"reference_id")||!num(d,0,"uncertainty_db",&n)||n<0||!num(d,0,"low_hz",&low)||!num(d,0,"high_hz",&high)||low<0||low>=high||high>500||!positive(d,0,"pa_per_fs")||!u64(d,0,"date_map_id",&map)||!map)return false;
 int curve=FIELD(d,0,"response_correction");if(curve<0||d->token[curve].type!=MJ_ARRAY||d->token[curve].count>32)return false;
 double previous=-1;for(unsigned k=0;k<d->token[curve].count;k++){int point=metadata_json_index(d,curve,k);double hz,gain;if(!array(d,point,2)||!metadata_json_double(d,metadata_json_index(d,point,0),&hz)||!metadata_json_double(d,metadata_json_index(d,point,1),&gain)||hz<low||hz>high||hz<=previous||gain<=0)return false;
 previous=hz;}
 m->calibrated=true;m->calibration_id=map;return true;
}
static bool time_map(const MetadataJson*d,StorageMetadata*m){
 int64_t utc;double slope;uint64_t raw;
 if(!m->identity.capture_id||!m->identity.config_id||!u64(d,0,"map_id",&m->object_id)||!m->object_id||!u64(d,0,"anchor_raw",&raw)||!u64(d,0,"valid_from_raw",&m->origin_raw)||raw!=m->origin_raw||m->identity.first_raw!=raw||m->identity.end_raw!=raw||!num(d,0,"slope_ns_per_raw",&slope)||slope<=0||!u64(d,0,"uncertainty_ns",&m->uncertainty)||!idfield(d,0,"clock_source")||!metadata_json_bool(d,FIELD(d,0,"synchronized"),&m->synchronized))return false;
 if(m->synchronized){if(!metadata_json_i64(d,FIELD(d,0,"utc_ns"),&utc))return false;}else if(!null(d,FIELD(d,0,"utc_ns")))return false;
 m->slope=slope;return true;
}
static bool fault(const MetadataJson*d,StorageMetadata*m){
 uint64_t start,end,count;bool exact_ranges;uint32_t stream;
 if(!u64(d,0,"fault_id",&m->object_id)||!m->object_id||!idfield(d,0,"component")||!idfield(d,0,"reason")||!u32(d,0,"stream_mask",&stream)||stream>15||!u64(d,0,"epoch",&count)||!u64(d,0,"count_delta",&count)||!u64(d,0,"monotonic_start_ns",&start)||!u64(d,0,"monotonic_end_ns",&end)||start>end||!metadata_json_bool(d,FIELD(d,0,"ranges_exact"),&exact_ranges)||!u64(d,0,"overflow_count",&count)||(count&&exact_ranges))return false;
 int ranges=FIELD(d,0,"record_seq_ranges");if(ranges<0||d->token[ranges].type!=MJ_ARRAY||d->token[ranges].count>STORAGE_UNSAVED_RANGES)return false;
 uint64_t previous=0;for(unsigned k=0;k<d->token[ranges].count;k++){int a=metadata_json_index(d,ranges,k);if(!array(d,a,2)||!metadata_json_u64(d,metadata_json_index(d,a,0),&start)||!metadata_json_u64(d,metadata_json_index(d,a,1),&end)||!start||start>=end||start<previous)return false;
 previous=end;}
 int samples=FIELD(d,0,"sample_ranges");if(null(d,samples))return !m->identity.first_raw&&!m->identity.end_raw;
 if(!array(d,samples,1)||!m->identity.capture_id||!m->identity.config_id)return false;
 int a=metadata_json_index(d,samples,0);
 return array(d,a,2)&&metadata_json_u64(d,metadata_json_index(d,a,0),&start)&&metadata_json_u64(d,metadata_json_index(d,a,1),&end)&&start<end&&start==m->identity.first_raw&&end==m->identity.end_raw;
}
bool storage_metadata_decode(const RecordIdentity*i,const uint8_t*b,size_t n,MetadataJson*scratch,StorageMetadata*out){
 if(!i||!scratch||!out||!metadata_json_parse(scratch,b,n))return false;
 StorageMetadata m={0};m.identity=*i;if(!common(scratch,i,&m))return false;
 bool ok=false;
 switch(i->type){
 case RECORD_SESSION:{bool temporary;uint64_t generation;
  ok=!i->capture_id&&!i->config_id&&!i->first_raw&&!i->end_raw&&metadata_json_bool(scratch,FIELD(scratch,0,"temporary"),&temporary)&&u64(scratch,0,"boot_generation",&generation)&&(temporary||generation)&&hash(scratch,FIELD(scratch,0,"build_manifest_hash"))&&u64(scratch,0,"start_monotonic_ns",&m.start_ns);break;}
 case RECORD_CONFIG:ok=config(scratch,&m);break;
 case RECORD_CAPTURE:ok=capture(scratch,&m);break;
 case RECORD_CALIBRATION:ok=calibration(scratch,&m);break;
 case RECORD_TIME_MAP:ok=time_map(scratch,&m);break;
 case RECORD_FAULT:ok=fault(scratch,&m);break;
 default:break;
 }
 if(ok)*out=m;
 return ok;
}
bool storage_metadata_record(const RecordIdentity*i,const uint8_t*json,size_t n,MetadataJson*scratch,StorageRequest*out,uint8_t*dest,size_t cap){
 StorageMetadata parsed;if(!out||!dest||n>cap||!storage_metadata_decode(i,json,n,scratch,&parsed))return false;
 memmove(dest,json,n);StorageRequest r={0};r.identity=*i;r.identity.record_seq=0;r.payload=dest;r.payload_bytes=n;*out=r;return true;
}

static bool same_config(const PlCaptureConfig*a,const PlCaptureConfig*b){return a->mode==b->mode&&a->source==b->source&&a->channel==b->channel&&a->pattern==b->pattern&&a->config_id==b->config_id;}
static const StorageMetadataEntry *find(const StorageMetadataRegistry*r,unsigned count,RecordType type,uint64_t session,uint64_t capture,uint32_t config,uint64_t object,uint64_t before){
 const StorageMetadataEntry*found=NULL;
 for(unsigned k=0;k<count;k++){
  const StorageMetadataEntry*e=&r->entries[k];const StorageMetadata*m=&e->value;
  if(m->identity.type!=type||m->identity.session_id!=session||m->identity.record_seq>=before)continue;
  if((type==RECORD_CAPTURE||type==RECORD_TIME_MAP)&&capture&&m->identity.capture_id!=capture)continue;
  if(type!=RECORD_SESSION&&type!=RECORD_TIME_MAP&&config&&m->identity.config_id!=config)continue;
  if((type==RECORD_CALIBRATION||type==RECORD_TIME_MAP)&&object&&m->object_id!=object)continue;
  if(!found||m->revision>found->value.revision||m->identity.record_seq>found->value.identity.record_seq)found=e;
 }return found;
}
static bool immutable_capture(const StorageMetadata*a,const StorageMetadata*b){
 return a->schema_version==b->schema_version&&a->time_pair.before_us==b->time_pair.before_us&&a->time_pair.after_us==b->time_pair.after_us&&a->time_pair.a_seq==b->time_pair.a_seq&&a->time_pair.b_seq==b->time_pair.b_seq&&same_config(&a->config,&b->config)&&a->origin_raw==b->origin_raw&&a->first_valid_raw==b->first_valid_raw&&a->start_ns==b->start_ns&&a->mono_raw==b->mono_raw&&a->mono_ns==b->mono_ns&&a->slope==b->slope&&a->uncertainty==b->uncertainty&&a->mask==b->mask&&!memcmp(a->first_seq,b->first_seq,sizeof(a->first_seq))&&!memcmp(a->first_raw,b->first_raw,sizeof(a->first_raw));
}
static StorageMetaResult references(const StorageMetadataRegistry*r,unsigned count,const StorageMetadata*m){
 const RecordIdentity*i=&m->identity;uint64_t before=i->record_seq?i->record_seq:UINT64_MAX;
 if(i->type==RECORD_SESSION)return META_OK;
 const StorageMetadataEntry*session=find(r,count,RECORD_SESSION,i->session_id,0,0,0,before);
 if(!session||strcmp(m->device_id,session->value.device_id))return META_REFERENCE;
 if(i->type==RECORD_CONFIG)return META_OK;
 const StorageMetadataEntry*cfg=i->config_id?find(r,count,RECORD_CONFIG,i->session_id,0,i->config_id,0,before):NULL;
 if(i->config_id&&!cfg)return META_REFERENCE;
 if(i->type==RECORD_CALIBRATION){
  if(!cfg||strcmp(m->hardware_id,cfg->value.hardware_id)||strcmp(m->gain_setting_id,cfg->value.gain_setting_id))return META_REFERENCE;
  if(m->calibrated&&(!cfg->value.gain_verified||cfg->value.config.source!=PL_CAPTURE_ADC||cfg->value.calibration_id!=m->object_id||!find(r,count,RECORD_TIME_MAP,i->session_id,0,0,m->calibration_id,before)))return META_REFERENCE;
 }
 if(i->type==RECORD_CAPTURE){
  if(!cfg||!same_config(&cfg->value.config,&m->config))return META_REFERENCE;
  if(m->schema_version==2&&m->start_ns<session->value.start_ns)return META_REFERENCE;
  const StorageMetadataEntry*old=find(r,count,RECORD_CAPTURE,i->session_id,i->capture_id,i->config_id,0,before);
  if(old&&(old->value.closed||old->value.revision==UINT64_MAX||m->revision!=old->value.revision+1||!immutable_capture(&old->value,m)))return META_CONFLICT;
  if(!old&&m->revision!=1)return META_CONFLICT;
 }
 if(i->type==RECORD_TIME_MAP){
  const StorageMetadataEntry*cap=find(r,count,RECORD_CAPTURE,i->session_id,i->capture_id,i->config_id,0,before);
  if(!cap||m->origin_raw<cap->value.origin_raw||(cap->value.closed&&m->origin_raw>cap->value.identity.end_raw))return META_REFERENCE;
  const StorageMetadataEntry*old=find(r,count,RECORD_TIME_MAP,i->session_id,i->capture_id,0,0,before);
  if(old&&(m->origin_raw<=old->value.origin_raw||m->object_id<=old->value.object_id))return META_CONFLICT;
 }
 if(i->type==RECORD_FAULT&&i->capture_id){
  const StorageMetadataEntry*cap=find(r,count,RECORD_CAPTURE,i->session_id,i->capture_id,i->config_id,0,before);
  if(!cap||!i->config_id||(i->first_raw&&i->first_raw<cap->value.origin_raw)||(cap->value.closed&&i->end_raw>cap->value.identity.end_raw))return META_REFERENCE;
 }
 return META_OK;
}
void storage_registry_init(StorageMetadataRegistry*r){if(r){memset(r,0,sizeof(*r));atomic_init(&r->published,0);}}
static bool same_key(const StorageMetadata*a,const StorageMetadata*b){
 const RecordIdentity*i=&b->identity;bool key=a->identity.type==i->type&&a->identity.session_id==i->session_id;
 if(i->type==RECORD_CONFIG)key=key&&a->identity.config_id==i->config_id;
 if(i->type==RECORD_CAPTURE)key=key&&a->identity.capture_id==i->capture_id&&a->identity.config_id==i->config_id&&a->revision==b->revision;
 if(i->type==RECORD_CALIBRATION||i->type==RECORD_TIME_MAP)key=key&&a->object_id==b->object_id;
 return i->type!=RECORD_FAULT&&key;
}
StorageMetaResult storage_registry_preflight(const StorageMetadataRegistry*r,const RecordIdentity*i,const uint8_t*b,size_t n,MetadataJson*scratch){
 StorageMetadata m;if(!r||!i||!i->record_seq||i->record_seq==UINT64_MAX||!storage_metadata_decode(i,b,n,scratch,&m))return META_INVALID;
 unsigned count=atomic_load_explicit(&r->published,memory_order_acquire);
 for(unsigned k=0;k<count;k++){
  const StorageMetadataEntry*e=&r->entries[k];if(!same_key(&e->value,&m))continue;
  if(e->bytes!=n||memcmp(e->payload,b,n))return META_CONFLICT;
  if(i->record_seq>=e->value.identity.record_seq)return META_OK;
 }
 StorageMetaResult result=references(r,count,&m);if(result!=META_OK)return result;
 return i->type!=RECORD_FAULT&&count==STORAGE_METADATA_ENTRIES?META_CAPACITY:META_OK;
}
StorageMetaResult storage_registry_apply(StorageMetadataRegistry*r,const RecordIdentity*i,const uint8_t*b,size_t n,MetadataJson*scratch){
 StorageMetadata m;if(!r||!i||!i->record_seq||i->record_seq==UINT64_MAX||!storage_metadata_decode(i,b,n,scratch,&m))return META_INVALID;
 unsigned count=atomic_load_explicit(&r->published,memory_order_acquire);
 for(unsigned k=0;k<count;k++){
  const StorageMetadataEntry*e=&r->entries[k];const StorageMetadata*a=&e->value;
  if(same_key(a,&m)){if(e->bytes!=n||memcmp(e->payload,b,n))return META_CONFLICT;if(i->record_seq>=a->identity.record_seq)return META_OK;}
 }
 StorageMetaResult result=references(r,count,&m);if(result!=META_OK)return result;
 if(i->type==RECORD_FAULT)return META_OK;
 if(count==STORAGE_METADATA_ENTRIES)return META_CAPACITY;
 StorageMetadataEntry*e=&r->entries[count];e->value=m;e->bytes=n;memcpy(e->payload,b,n);
 atomic_store_explicit(&r->published,count+1,memory_order_release);return META_OK;
}
StorageMetaResult storage_registry_policy(const StorageMetadataRegistry*r,uint64_t session,uint64_t capture_id,uint32_t config_id,uint64_t before,StoragePolicyCopy*out){
 if(!r||!out||!session||!capture_id||!config_id)return META_INVALID;
 if(!before)before=UINT64_MAX;
 unsigned count=atomic_load_explicit(&r->published,memory_order_acquire);
 const StorageMetadataEntry*s=find(r,count,RECORD_SESSION,session,0,0,0,before),*cfg=find(r,count,RECORD_CONFIG,session,0,config_id,0,before),*cap=find(r,count,RECORD_CAPTURE,session,capture_id,config_id,0,before);
 if(!s||!cfg||!cap)return META_REFERENCE;
 if(cfg->value.calibration_id){const StorageMetadataEntry*cal=find(r,count,RECORD_CALIBRATION,session,0,config_id,cfg->value.calibration_id,before);if(!cal||!cal->value.calibrated)return META_REFERENCE;}
 StoragePolicyCopy copy={0};memcpy(copy.device_id,s->value.device_id,sizeof(copy.device_id));copy.policy.session_id=session;copy.policy.capture_id=capture_id;copy.policy.config=cfg->value.config;copy.policy.bound_mask=cap->value.mask;
 memcpy(copy.policy.first_seq,cap->value.first_seq,sizeof(copy.policy.first_seq));memcpy(copy.policy.first_raw,cap->value.first_raw,sizeof(copy.policy.first_raw));
 const StorageMetadataEntry*latest=find(r,count,RECORD_CAPTURE,session,capture_id,config_id,0,UINT64_MAX);copy.closed=latest&&latest->value.closed;copy.end_raw=copy.closed?latest->value.identity.end_raw:UINT64_MAX;
 *out=copy;out->policy.device_id=out->device_id;return META_OK;
}
StorageMetaResult storage_registry_capture(const StorageMetadataRegistry *r,uint64_t session,uint64_t capture_id,uint32_t config_id,uint64_t revision,uint64_t before,StorageMetadata *out){
 if(!r||!out||!session||!capture_id||!config_id||!revision)return META_INVALID;
 if(!before)before=UINT64_MAX;
 unsigned count=atomic_load_explicit(&r->published,memory_order_acquire);
 for(unsigned k=0;k<count;k++){const StorageMetadata *m=&r->entries[k].value;
  if(m->identity.type==RECORD_CAPTURE&&m->identity.session_id==session&&m->identity.capture_id==capture_id&&m->identity.config_id==config_id&&m->revision==revision&&m->identity.record_seq<before){*out=*m;return META_OK;}
 }return META_REFERENCE;
}
static bool registry_summary(const StorageMetadataRegistry *r,const RecordIdentity *id,const uint8_t *b,size_t n){
 MetadataJson scratch;MinuteSummary m;if(!storage_summary_decode(id,b,n,&scratch,&m))return false;
 unsigned count=atomic_load_explicit(&r->published,memory_order_acquire);uint64_t before=id->record_seq?id->record_seq:UINT64_MAX;
 const StorageMetadataEntry *session=find(r,count,RECORD_SESSION,id->session_id,0,0,0,before),*cfg=find(r,count,RECORD_CONFIG,id->session_id,0,id->config_id,0,before),*cap=find(r,count,RECORD_CAPTURE,id->session_id,id->capture_id,id->config_id,0,before);
 StorageMetadata exact_cap;
 if(m.schema_version==2){
  if(!cfg||!cfg->value.summary_histogram)return false;
  if(!session||storage_registry_capture(r,id->session_id,id->capture_id,id->config_id,m.capture_revision,before,&exact_cap)!=META_OK||exact_cap.time_quality!=STORAGE_TIME_NOMINAL_PAIRED||session->value.start_ns!=m.session_start_ns)return false;
  const CaptureTimePair *a=&m.time_pair,*c=&exact_cap.time_pair;
  if(a->anchor_raw!=c->anchor_raw||a->anchor_ns!=c->anchor_ns||a->uncertainty_ns!=c->uncertainty_ns||a->before_us!=c->before_us||a->after_us!=c->after_us||a->a_seq!=c->a_seq||a->b_seq!=c->b_seq||m.first_raw<exact_cap.first_valid_raw||(m.first_raw-exact_cap.first_valid_raw)%48||m.observed_first_ns<exact_cap.start_ns||(exact_cap.closed&&m.end_raw>exact_cap.identity.end_raw))return false;
 }
 if(!session||!cfg||!cap||cfg->value.config.mode!=0||m.identity.test_source!=(cfg->value.config.source==PL_CAPTURE_TEST)||strcmp(session->value.device_id,m.identity.device_id)||m.observed_first_ns<cap->value.start_ns||(m.raw_known&&(m.first_raw<cap->value.origin_raw||(cap->value.closed&&m.end_raw>cap->value.identity.end_raw))))return false;
 char band[64];return metadata_json_parse(&scratch,cfg->payload,cfg->bytes)&&identifier(&scratch,FIELD(&scratch,0,"band_definition_id"),band,sizeof(band))&&!strcmp(band,m.identity.band_definition_id);
}
bool storage_registry_validate(void*context,const RecordIdentity*i,const uint8_t*b,size_t n){
 const StorageMetadataRegistry*r=context;if(!r||!i)return false;
 if(i->type==RECORD_MARKER){
  MetadataJson scratch;LfnMarker m;StoragePolicyCopy p;char device[64];
  if(!storage_marker_decode(i,b,n,&scratch,&m)||storage_registry_policy(r,i->session_id,i->capture_id,i->config_id,i->record_seq,&p)!=META_OK||
   !metadata_json_string(&scratch,metadata_json_field(&scratch,0,"device_id"),device,sizeof(device))||strcmp(device,p.device_id))return false;
  unsigned count=atomic_load_explicit(&r->published,memory_order_acquire);uint64_t before=i->record_seq?i->record_seq:UINT64_MAX;
  const StorageMetadataEntry *cap=find(r,count,RECORD_CAPTURE,i->session_id,i->capture_id,i->config_id,0,before);
  return cap&&m.start.raw_index>=cap->value.origin_raw&&(!p.closed||i->end_raw<=p.end_raw);
 }
 if(i->type==RECORD_MINUTE_SUMMARY)return registry_summary(r,i,b,n);
 if(i->type==RECORD_EVENT_META){
  MetadataJson scratch;StorageEventMeta m;StoragePolicyCopy p;
  if(!storage_event_decode(i,b,n,&scratch,&m)||storage_registry_policy(r,i->session_id,i->capture_id,i->config_id,i->record_seq,&p)!=META_OK||p.policy.config.mode||strcmp(p.device_id,m.device_id)||m.actual_first<p.policy.first_raw[1]||(m.actual_first-p.policy.first_raw[1])%48||(p.closed&&m.end_raw>p.end_raw))return false;
  return true;
 }
 if(i->type==RECORD_ENERGY||i->type==RECORD_PSD||i->type==RECORD_PCM){StoragePolicyCopy p;if(storage_registry_policy(r,i->session_id,i->capture_id,i->config_id,i->record_seq,&p)!=META_OK||(p.closed&&i->end_raw>p.end_raw))return false;
 return storage_data_validate(&p.policy,i,b,n);}
 MetadataJson scratch;StorageMetadata m;if(!storage_metadata_decode(i,b,n,&scratch,&m))return false;
 unsigned count=atomic_load_explicit(&r->published,memory_order_acquire);
 /* Exact replays/copies are valid without being a new state transition. */
 for(unsigned k=0;k<count;k++){
  const StorageMetadataEntry*e=&r->entries[k];if(!same_key(&e->value,&m))continue;
  if(e->bytes!=n||memcmp(e->payload,b,n))return false;
  if(!i->record_seq||i->record_seq>=e->value.identity.record_seq)return true;
 }
 return references(r,count,&m)==META_OK;
}
static bool adjacent(const StorageUnsavedRange*a,const StorageUnsavedRange*b){
 return a->end_sequence==b->first_sequence&&a->session_id==b->session_id&&a->capture_id==b->capture_id&&a->config_id==b->config_id&&a->sample_range_known==b->sample_range_known&&(!a->sample_range_known||a->end_raw==b->first_raw);
}
bool storage_unsaved_add(StorageUnsavedLedger*l,const StorageUnsavedRange*r){
 if(!l||!r||l->count>STORAGE_UNSAVED_RANGES||!r->session_id||!r->first_sequence||r->first_sequence>=r->end_sequence||(r->sample_range_known&&(!r->capture_id||!r->config_id||r->first_raw>=r->end_raw)))return false;
 unsigned at=0;
 for(unsigned k=0;k<l->count;k++){
  const StorageUnsavedRange*x=&l->ranges[k];if(x->session_id!=r->session_id||!(r->end_sequence<=x->first_sequence||r->first_sequence>=x->end_sequence))return false;
  if(x->first_sequence<r->first_sequence)at++;
 }
 if(at&&adjacent(&l->ranges[at-1],r)){
  StorageUnsavedRange*x=&l->ranges[at-1];x->end_sequence=r->end_sequence;x->end_raw=r->end_raw;
  if(at<l->count&&adjacent(x,&l->ranges[at])){x->end_sequence=l->ranges[at].end_sequence;x->end_raw=l->ranges[at].end_raw;memmove(&l->ranges[at],&l->ranges[at+1],(l->count-at-1)*sizeof(*r));l->count--;}
  return true;
 }
 if(at<l->count&&adjacent(r,&l->ranges[at])){l->ranges[at].first_sequence=r->first_sequence;l->ranges[at].first_raw=r->first_raw;return true;}
 if(l->count==STORAGE_UNSAVED_RANGES){if(l->overflow!=UINT64_MAX)l->overflow++;return false;}
 memmove(&l->ranges[at+1],&l->ranges[at],(l->count-at)*sizeof(*r));l->ranges[at]=*r;l->count++;return true;
}
bool storage_unsaved_record(const char*device,uint64_t session,uint64_t fault_id,uint64_t start,uint64_t end,const StorageUnsavedLedger*l,MetadataJson*scratch,StorageRequest*out,uint8_t*dest,size_t cap){
 if(!device||!l||!l->count||l->count>STORAGE_UNSAVED_RANGES||!session||!fault_id||start>end)return false;
 size_t length=0;for(;length<64&&device[length];length++){unsigned char c=(unsigned char)device[length];if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.'))return false;}
 if(!length||length==64)return false;
 uint64_t delta=0;for(unsigned k=0;k<l->count;k++){uint64_t span=l->ranges[k].end_sequence-l->ranges[k].first_sequence;if(delta>UINT64_MAX-span)return false;
 delta+=span;}
 char b[2048];bool known=l->count==1&&l->ranges[0].sample_range_known;uint64_t capture_id=known?l->ranges[0].capture_id:0;uint32_t config_id=known?l->ranges[0].config_id:0;
 char capture[24],config[16],samples[64];
 if(known){(void)snprintf(capture,sizeof(capture),"\"%llu\"",(unsigned long long)capture_id);(void)snprintf(config,sizeof(config),"%u",(unsigned)config_id);(void)snprintf(samples,sizeof(samples),"[[\"%llu\",\"%llu\"]]",(unsigned long long)l->ranges[0].first_raw,(unsigned long long)l->ranges[0].end_raw);}
 else{memcpy(capture,"null",5);memcpy(config,"null",5);memcpy(samples,"null",5);}
 int n=snprintf(b,sizeof(b),"{\"schema_version\":1,\"device_id\":\"%s\",\"session_id\":\"%llu\",\"capture_id\":%s,\"config_id\":%s,\"fault_id\":\"%llu\",\"component\":\"storage\",\"reason\":\"UNSAVED\",\"stream_mask\":0,\"epoch\":\"0\",\"count_delta\":\"%llu\",\"monotonic_start_ns\":\"%llu\",\"monotonic_end_ns\":\"%llu\",\"ranges_exact\":%s,\"overflow_count\":\"%llu\",\"sample_ranges\":%s,\"record_seq_ranges\":[",device,(unsigned long long)session,capture,config,(unsigned long long)fault_id,(unsigned long long)delta,(unsigned long long)start,(unsigned long long)end,l->overflow?"false":"true",(unsigned long long)l->overflow,samples);
 if(n<0||(size_t)n>=sizeof(b))return false;
 size_t used=(size_t)n;
 for(unsigned k=0;k<l->count;k++){const StorageUnsavedRange*r=&l->ranges[k];if(r->session_id!=session)return false;
  n=snprintf(b+used,sizeof(b)-used,"%s[\"%llu\",\"%llu\"]",k?",":"",(unsigned long long)r->first_sequence,(unsigned long long)r->end_sequence);if(n<0||(size_t)n>=sizeof(b)-used)return false;
 used+=(size_t)n;}
 if(used+2>=sizeof(b))return false;
 b[used++]=']';b[used++]='}';RecordIdentity id={RECORD_FAULT,0,config_id,session,capture_id,known?l->ranges[0].first_raw:0,known?l->ranges[0].end_raw:0,0};
 return storage_metadata_record(&id,(const uint8_t*)b,used,scratch,out,dest,cap);
}

static bool summary_identifier(const char *s){
 if(!s[0]||!memchr(s,0,64))return false;
 for(unsigned k=0;k<64&&s[k];k++){unsigned char c=(unsigned char)s[k];if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.'))return false;}return true;
}
static bool summary_consistent(const MinuteSummary *m){
 if(m&&m->schema_version==2){
  const CaptureTimePair *p=&m->time_pair;uint64_t first,end;
  if(!capture_time_nominal_valid(p)||m->identity.test_source!=(p->source==PL_CAPTURE_TEST)||p->session_id!=m->identity.session_id||p->capture_id!=m->identity.capture_id||p->config_id!=m->identity.config_id||!m->capture_revision||!m->raw_known||m->bucket_start_ns<m->session_start_ns||m->bucket_end_ns<=m->bucket_start_ns||(m->bucket_end_ns-m->bucket_start_ns)%1000000||m->quantization_start_ns>=1000000||m->quantization_end_ns>=1000000||m->bucket_end_ns>UINT64_MAX-m->quantization_end_ns||m->first_ns<m->bucket_start_ns+m->quantization_start_ns||m->end_ns>m->bucket_end_ns+m->quantization_end_ns||m->observed_first_ns!=m->first_ns||!capture_time_pair_map_ns(p,m->first_raw,&first)||!capture_time_pair_map_ns(p,m->end_raw,&end)||first!=m->first_ns||end!=m->end_ns||(m->end_raw-m->first_raw)%48||m->expected_frames||m->valid_frames||m->valid_candidates>m->expected_candidates||m->pcm_saved_ticks>m->pcm_valid_ticks||m->pcm_saved_ticks%48||m->pcm_valid_ticks%48||m->available_ns>m->requested_ns||m->source_available_unknown_ns>m->requested_ns-m->available_ns||m->unavailable_ns!=m->requested_ns-m->available_ns-m->source_available_unknown_ns||m->valid_ns>m->requested_ns||m->statistic_unknown_ns>m->requested_ns-m->valid_ns||m->requested_ns%1000000||m->available_ns%1000000||m->valid_ns%1000000||m->unavailable_ns%1000000||m->statistic_unknown_ns%1000000||m->source_available_unknown_ns%1000000)return false;
  if(m->quantization_start_ns!=(m->first_ns-m->bucket_start_ns)%1000000||m->quantization_end_ns!=m->quantization_start_ns||(m->bucket_start_ns-m->session_start_ns)%(m->bucket_end_ns-m->bucket_start_ns))return false;
  if(!m->partial&&(m->first_ns!=m->bucket_start_ns+m->quantization_start_ns||m->end_ns!=m->bucket_end_ns+m->quantization_end_ns))return false;
 }else if(m&&m->schema_version>1)return false;
 if(!m||!m->identity.session_id||!m->identity.capture_id||!m->identity.config_id||!summary_identifier(m->identity.device_id)||!summary_identifier(m->identity.band_definition_id)||!m->summary_id||!m->revision||m->first_ns>=m->end_ns||m->observed_first_ns<m->first_ns||m->observed_first_ns>=m->end_ns||m->requested_ns>m->end_ns-m->observed_first_ns||m->available_ns>m->requested_ns||m->valid_ns>m->available_ns||(m->schema_version!=2&&m->unavailable_ns!=m->requested_ns-m->available_ns)||m->unsaved_ns>m->valid_ns||!isfinite(m->expected_frames)||!isfinite(m->valid_frames)||m->expected_frames<0||m->valid_frames<0||m->valid_frames>m->expected_frames||m->first_raw>m->end_raw||(!m->raw_known&&(m->first_raw||m->end_raw||m->pcm_valid_ticks))||(m->raw_known&&m->pcm_valid_ticks>m->end_raw-m->first_raw)||m->event_count>32||m->marker_count>32||m->event_total<m->event_count||m->marker_total<m->marker_count||(!m->references_truncated&&(m->event_total!=m->event_count||m->marker_total!=m->marker_count)))return false;
 for(unsigned k=0;k<10;k++){if(!isfinite(m->sum_power_weighted[k])||m->sum_power_weighted[k]<0||(!m->valid_ns&&m->sum_power_weighted[k]))return false;
 uint64_t total=0;for(unsigned j=0;j<26;j++){if(m->histogram_ns[k][j]>UINT64_MAX-total)return false;
 total+=m->histogram_ns[k][j];}if(total!=m->valid_ns)return false;}
 for(unsigned which=0;which<2;which++){unsigned count=which?m->marker_count:m->event_count;const uint64_t *refs=which?m->markers:m->events;for(unsigned i=0;i<count;i++){if(!refs[i])return false;
 for(unsigned j=0;j<i;j++)if(refs[j]==refs[i])return false;}}
 return true;
}
bool storage_summary_decode(const RecordIdentity *id,const uint8_t *b,size_t n,MetadataJson *d,MinuteSummary *out){
 if(!id||id->type!=RECORD_MINUTE_SUMMARY||id->flags||!d||!out||!metadata_json_parse(d,b,n)||(!exact(d,0,"schema_version",1)&&!exact(d,0,"schema_version",2)))return false;
 MinuteSummary m={0};
 m.schema_version=exact(d,0,"schema_version",2)?2:1;
 if(!identifier(d,FIELD(d,0,"device_id"),m.identity.device_id,64)||!identifier(d,FIELD(d,0,"band_definition_id"),m.identity.band_definition_id,64)||!u64(d,0,"session_id",&m.identity.session_id)||!u64(d,0,"capture_id",&m.identity.capture_id)||!u32(d,0,"config_id",&m.identity.config_id)||m.identity.session_id!=id->session_id||m.identity.capture_id!=id->capture_id||m.identity.config_id!=id->config_id)return false;
 if(!metadata_json_bool(d,FIELD(d,0,"test_source"),&m.identity.test_source)||!metadata_json_bool(d,FIELD(d,0,"raw_known"),&m.raw_known)||!metadata_json_bool(d,FIELD(d,0,"partial"),&m.partial)||!metadata_json_bool(d,FIELD(d,0,"references_truncated"),&m.references_truncated)||!text(d,FIELD(d,0,"power_unit"),"FS2")||!text(d,FIELD(d,0,"histogram_unit"),"ns_5dBFS_bins"))return false;
 struct SummaryU64 {const char *key;uint64_t *value;} fields[]={
 {"summary_id",&m.summary_id},{"revision",&m.revision},{"summary_start_monotonic_ns",&m.first_ns},{"summary_end_monotonic_ns",&m.end_ns},{"observed_first_ns",&m.observed_first_ns},{"requested_ns",&m.requested_ns},{"available_ns",&m.available_ns},{"weight_total",&m.valid_ns},{"unavailable_ns",&m.unavailable_ns},{"unsaved_ns",&m.unsaved_ns},{"pcm_valid_ticks",&m.pcm_valid_ticks},{"first_raw",&m.first_raw},{"end_raw",&m.end_raw},{"event_total",&m.event_total},{"marker_total",&m.marker_total}};
 for(unsigned k=0;k<sizeof(fields)/sizeof(fields[0]);k++)if(!u64(d,0,fields[k].key,fields[k].value))return false;
 uint32_t flags=0;if(!u32(d,0,"source_flags",&flags)||flags>65535||(m.schema_version!=2&&(!num(d,0,"expected_frame_count",&m.expected_frames)||!num(d,0,"valid_frame_count",&m.valid_frames)))||m.first_raw!=id->first_raw||m.end_raw!=id->end_raw)return false;
 m.flags=(uint16_t)flags;
 if(m.schema_version==2){
  if(!equal_number(d,0,"histogram_reference_power_fs2",0.5)||!equal_number(d,0,"histogram_min_dbfs",-120)||!exact(d,0,"histogram_step_db",5)||!exact(d,0,"histogram_bin_count",26))return false;
  if(!text(d,FIELD(d,0,"coverage_basis"),"NOMINAL_GRID")||!text(d,FIELD(d,0,"time_quality"),"NOMINAL_PAIRED")||!pair_method(d,m.identity.test_source?PL_CAPTURE_TEST:PL_CAPTURE_ADC)||!text(d,FIELD(d,0,"weighted_power_unit"),"FS2_S")||!text(d,FIELD(d,0,"weight_unit"),"ns")||!null(d,FIELD(d,0,"frequency_error_ppm")))return false;
  CaptureTimePair *p=&m.time_pair;p->status=CAPTURE_TIME_CONFIRMED_NOMINAL;p->session_id=id->session_id;p->capture_id=id->capture_id;p->config_id=id->config_id;p->source=m.identity.test_source?PL_CAPTURE_TEST:PL_CAPTURE_ADC;p->nominal_rate_hz=48000;
  struct SummaryU64 extra[]={
   {"capture_revision",&m.capture_revision},{"session_start_ns",&m.session_start_ns},{"bucket_start_ns",&m.bucket_start_ns},{"bucket_end_ns",&m.bucket_end_ns},
   {"quantization_start_ns",&m.quantization_start_ns},{"quantization_end_ns",&m.quantization_end_ns},{"expected_frame_count",&m.expected_candidates},{"valid_frame_count",&m.valid_candidates},{"pcm_saved_ticks",&m.pcm_saved_ticks},
   {"statistic_unknown_ns",&m.statistic_unknown_ns},{"source_available_unknown_ns",&m.source_available_unknown_ns},
   {"mono_anchor_raw",&p->anchor_raw},{"mono_anchor_ns",&p->anchor_ns},{"mono_uncertainty_ns",&p->uncertainty_ns},{"pair_before_us",&p->before_us},{"pair_after_us",&p->after_us}};
  for(unsigned k=0;k<sizeof(extra)/sizeof(extra[0]);k++)if(!u64(d,0,extra[k].key,extra[k].value))return false;
  uint64_t numerator,denominator;
  if(!u32(d,0,"pair_a_seq",&p->a_seq)||!u32(d,0,"pair_b_seq",&p->b_seq)||!u64(d,0,"nominal_ns_numerator",&numerator)||numerator!=1000000000||!u64(d,0,"nominal_raw_denominator",&denominator)||denominator!=48000)return false;
 }
 int powers=FIELD(d,0,"sum_power_weighted"),hist=FIELD(d,0,"histogram_ns");if(!array(d,powers,10)||!array(d,hist,10))return false;
 for(unsigned k=0;k<10;k++){if(!metadata_json_double(d,metadata_json_index(d,powers,k),&m.sum_power_weighted[k]))return false;
 int row=metadata_json_index(d,hist,k);if(!array(d,row,26))return false;
 for(unsigned j=0;j<26;j++)if(!metadata_json_u64(d,metadata_json_index(d,row,j),&m.histogram_ns[k][j]))return false;}
 for(unsigned which=0;which<2;which++){int refs=FIELD(d,0,which?"marker_refs":"event_refs");if(refs<0||d->token[refs].type!=MJ_ARRAY||d->token[refs].count>32)return false;
 unsigned count=d->token[refs].count;uint64_t *values=which?m.markers:m.events;for(unsigned k=0;k<count;k++)if(!metadata_json_u64(d,metadata_json_index(d,refs,k),&values[k]))return false;
 if(which)m.marker_count=count;else m.event_count=count;}
 if(!summary_consistent(&m))return false;
 *out=m;return true;
}

static bool summary_append(uint8_t *b,size_t cap,size_t *used,const char *fmt,...){
 if(*used>=cap)return false;
 va_list args;va_start(args,fmt);int n=vsnprintf((char*)b+*used,cap-*used,fmt,args);va_end(args);if(n<0||(size_t)n>=cap-*used)return false;
 *used+=(size_t)n;return true;
}
bool storage_summary_record(const MinuteSummary *m,StorageRequest *out,uint8_t *b,size_t cap){
 if(!out||!b||!summary_consistent(m))return false;
 if(cap>STORAGE_METADATA_BYTES)cap=STORAGE_METADATA_BYTES;
 size_t used=0;
 if(!summary_append(b,cap,&used,"{\"schema_version\":%u,\"device_id\":\"%s\",\"band_definition_id\":\"%s\",\"session_id\":\"%llu\",\"capture_id\":\"%llu\",\"config_id\":%u,\"test_source\":%s,\"raw_known\":%s,\"partial\":%s,\"references_truncated\":%s,\"power_unit\":\"FS2\",\"histogram_unit\":\"ns_5dBFS_bins\",",m->schema_version==2?2u:1u,m->identity.device_id,m->identity.band_definition_id,(unsigned long long)m->identity.session_id,(unsigned long long)m->identity.capture_id,m->identity.config_id,m->identity.test_source?"true":"false",m->raw_known?"true":"false",m->partial?"true":"false",m->references_truncated?"true":"false"))return false;
 struct SummaryValue {const char *key;uint64_t value;} fields[]={
 {"summary_id",m->summary_id},{"revision",m->revision},{"summary_start_monotonic_ns",m->first_ns},{"summary_end_monotonic_ns",m->end_ns},{"observed_first_ns",m->observed_first_ns},{"requested_ns",m->requested_ns},{"available_ns",m->available_ns},{"weight_total",m->valid_ns},{"unavailable_ns",m->unavailable_ns},{"unsaved_ns",m->unsaved_ns},{"pcm_valid_ticks",m->pcm_valid_ticks},{"first_raw",m->first_raw},{"end_raw",m->end_raw},{"event_total",m->event_total},{"marker_total",m->marker_total}};
 for(unsigned k=0;k<sizeof(fields)/sizeof(fields[0]);k++)if(!summary_append(b,cap,&used,"\"%s\":\"%llu\",",fields[k].key,(unsigned long long)fields[k].value))return false;
 if(m->schema_version==2){
  if(!summary_append(b,cap,&used,"\"histogram_reference_power_fs2\":0.5,\"histogram_min_dbfs\":-120,\"histogram_step_db\":5,\"histogram_bin_count\":26,"))return false;
  const CaptureTimePair *p=&m->time_pair;
  if(!summary_append(b,cap,&used,"\"coverage_basis\":\"NOMINAL_GRID\",\"time_quality\":\"NOMINAL_PAIRED\",\"pair_method\":\"%s\",\"pair_cdc_guard_samples\":%u,\"weighted_power_unit\":\"FS2_S\",\"weight_unit\":\"ns\",\"frequency_error_ppm\":null,\"nominal_ns_numerator\":\"1000000000\",\"nominal_raw_denominator\":\"48000\",\"pair_a_seq\":%u,\"pair_b_seq\":%u,",capture_time_pair_method(p),p->source==PL_CAPTURE_ADC?1u:0u,p->a_seq,p->b_seq))return false;
  struct SummaryValue extra[]={
   {"capture_revision",m->capture_revision},{"session_start_ns",m->session_start_ns},{"bucket_start_ns",m->bucket_start_ns},{"bucket_end_ns",m->bucket_end_ns},
   {"quantization_start_ns",m->quantization_start_ns},{"quantization_end_ns",m->quantization_end_ns},{"pcm_saved_ticks",m->pcm_saved_ticks},{"statistic_unknown_ns",m->statistic_unknown_ns},{"source_available_unknown_ns",m->source_available_unknown_ns},
   {"mono_anchor_raw",p->anchor_raw},{"mono_anchor_ns",p->anchor_ns},{"mono_uncertainty_ns",p->uncertainty_ns},{"pair_before_us",p->before_us},{"pair_after_us",p->after_us}};
  for(unsigned k=0;k<sizeof(extra)/sizeof(extra[0]);k++)if(!summary_append(b,cap,&used,"\"%s\":\"%llu\",",extra[k].key,(unsigned long long)extra[k].value))return false;
 }
 if(m->schema_version==2){if(!summary_append(b,cap,&used,"\"expected_frame_count\":\"%llu\",\"valid_frame_count\":\"%llu\",",(unsigned long long)m->expected_candidates,(unsigned long long)m->valid_candidates))return false;}
 else if(!summary_append(b,cap,&used,"\"expected_frame_count\":%.17g,\"valid_frame_count\":%.17g,",m->expected_frames,m->valid_frames))return false;
 if(!summary_append(b,cap,&used,"\"source_flags\":%u,\"sum_power_weighted\":[",m->flags))return false;
 for(unsigned k=0;k<10;k++)if(!summary_append(b,cap,&used,"%s%.17g",k?",":"",m->sum_power_weighted[k]))return false;
 if(!summary_append(b,cap,&used,"],\"histogram_ns\":["))return false;
 for(unsigned k=0;k<10;k++){if(!summary_append(b,cap,&used,"%s[",k?",":""))return false;
 for(unsigned j=0;j<26;j++)if(!summary_append(b,cap,&used,"%s\"%llu\"",j?",":"",(unsigned long long)m->histogram_ns[k][j]))return false;
 if(!summary_append(b,cap,&used,"]"))return false;}
 for(unsigned which=0;which<2;which++){if(!summary_append(b,cap,&used,which?"],\"marker_refs\":[":"],\"event_refs\":["))return false;
 unsigned count=which?m->marker_count:m->event_count;const uint64_t *refs=which?m->markers:m->events;for(unsigned k=0;k<count;k++)if(!summary_append(b,cap,&used,"%s\"%llu\"",k?",":"",(unsigned long long)refs[k]))return false;}
 if(!summary_append(b,cap,&used,"]}"))return false;
 StorageRequest r={0};r.identity=(RecordIdentity){RECORD_MINUTE_SUMMARY,0,m->identity.config_id,m->identity.session_id,m->identity.capture_id,m->raw_known?m->first_raw:0,m->raw_known?m->end_raw:0,0};r.payload=b;r.payload_bytes=used;*out=r;return true;
}

static const char *event_states[]={"OPEN","APPEND","CLOSED","INCOMPLETE"};
static bool event_text(const char *s,size_t cap){
 if(!s||!*s||!memchr(s,0,cap))return false;
 for(size_t k=0;s[k];k++){unsigned char c=(unsigned char)s[k];if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.'))return false;}return true;
}
static bool event_consistent(const StorageEventMeta *m){
 if(!m||!event_text(m->device_id,64)||!event_text(m->close_reason,32)||!event_text(m->missing_ref_reason,32)||!event_text(m->evidence_reason,32)||!m->session_id||!m->capture_id||!m->config_id||!m->event_id||!m->persist_revision||(unsigned)m->state>EVENT_META_INCOMPLETE||m->trigger_count>32||m->pcm_count>32||m->gap_count>32||m->trigger_total<m->trigger_count||m->triggers_truncated!=(m->trigger_total>m->trigger_count)||m->prehistory_requested_first>m->actual_first||(!m->segment_no&&m->onset_raw<m->prehistory_requested_first)||m->actual_first>m->end_raw||m->append_cursor<m->actual_first||m->append_cursor>m->end_raw||(m->end_raw-m->actual_first)%48||(m->append_cursor-m->actual_first)%48||m->valid_sample_count>(m->append_cursor-m->actual_first)/48||m->prehistory_short!=(m->actual_first>m->prehistory_requested_first))return false;
 if(m->state==EVENT_META_OPEN){if(m->persist_revision!=1||m->prev_meta.file_id||m->prev_meta.record_seq||m->pcm_count||m->valid_sample_count||m->append_cursor!=m->actual_first||m->event_closed||strcmp(m->close_reason,"NONE"))return false;}
 else if(m->persist_revision==1||!m->prev_meta.file_id||!m->prev_meta.record_seq)return false;
 if(m->state==EVENT_META_APPEND&&(m->event_closed||strcmp(m->close_reason,"NONE")))return false;
 if(m->state==EVENT_META_CLOSED){if(m->append_cursor!=m->end_raw||m->tail_missing||m->gaps_truncated||strcmp(m->missing_ref_reason,"NONE")||!strcmp(m->close_reason,"NONE")||(!m->event_closed&&strcmp(m->close_reason,"SEGMENT_LIMIT"))||(m->event_closed&&!strcmp(m->close_reason,"SEGMENT_LIMIT")))return false;}
 if(m->state==EVENT_META_INCOMPLETE&&!strcmp(m->close_reason,"NONE"))return false;
 if(!m->evidence_count){if(m->evidence_first_raw||m->evidence_end_raw||!strcmp(m->evidence_reason,"NONE"))return false;}
 else if(m->evidence_first_raw>=m->evidence_end_raw)return false;
 for(unsigned k=0;k<m->trigger_count;k++)if(m->triggers[k]<m->onset_raw||m->triggers[k]>=m->end_raw||(k&&m->triggers[k]<=m->triggers[k-1]))return false;
 uint64_t added=0;
 for(unsigned k=0;k<m->pcm_count;k++){
  const StorageEventPcmRef *a=&m->pcm[k];if(!a->file_id||!a->record_seq||a->raw_stride!=48||!a->sample_count||a->sample_count>4096||a->first_raw<m->actual_first||a->first_raw>=a->end_raw||a->end_raw>m->append_cursor||(a->first_raw-m->actual_first)%48||a->end_raw-a->first_raw!=(uint64_t)a->sample_count*48)return false;
  added+=a->sample_count;
  for(unsigned j=0;j<k;j++){const StorageEventPcmRef *b=&m->pcm[j];if((a->file_id==b->file_id&&a->record_seq==b->record_seq)||(a->first_raw<b->end_raw&&b->first_raw<a->end_raw))return false;}
 }
 if(added>m->valid_sample_count)return false;
 for(unsigned k=0;k<m->gap_count;k++){
  const StorageEventGap *a=&m->gaps[k];if(a->first_raw<m->actual_first||a->first_raw>=a->end_raw||a->end_raw>m->end_raw||(a->first_raw-m->actual_first)%48||(a->end_raw-a->first_raw)%48)return false;
  for(unsigned j=0;j<k;j++)if(a->first_raw<m->gaps[j].end_raw&&m->gaps[j].first_raw<a->end_raw)return false;
  for(unsigned j=0;j<m->pcm_count;j++)if(a->first_raw<m->pcm[j].end_raw&&m->pcm[j].first_raw<a->end_raw)return false;
 }
 return true;
}
bool storage_event_decode(const RecordIdentity *id,const uint8_t *b,size_t n,MetadataJson *d,StorageEventMeta *out){
 if(!id||id->type!=RECORD_EVENT_META||id->flags||!d||!out||!metadata_json_parse(d,b,n)||!exact(d,0,"schema_version",1))return false;
 StorageEventMeta m={0};uint32_t segment;
 if(!identifier(d,FIELD(d,0,"device_id"),m.device_id,64)||!u32(d,0,"config_id",&m.config_id)||!u32(d,0,"segment_no",&segment))return false;
 m.segment_no=segment;
 struct {const char *name;uint64_t *v;} numbers[]={
 {"session_id",&m.session_id},{"capture_id",&m.capture_id},{"event_id",&m.event_id},{"persist_revision",&m.persist_revision},
 {"onset_raw",&m.onset_raw},{"prehistory_requested_first",&m.prehistory_requested_first},{"actual_first",&m.actual_first},{"end_raw",&m.end_raw},{"append_cursor",&m.append_cursor},{"trigger_total",&m.trigger_total},{"valid_sample_count",&m.valid_sample_count},
 {"evidence_count",&m.evidence_count},{"evidence_first_raw",&m.evidence_first_raw},{"evidence_end_raw",&m.evidence_end_raw}};
 for(unsigned k=0;k<sizeof(numbers)/sizeof(numbers[0]);k++)if(!u64(d,0,numbers[k].name,numbers[k].v))return false;
 struct {const char *name;bool *v;} booleans[]={{"event_closed",&m.event_closed},{"triggers_truncated",&m.triggers_truncated},{"gaps_truncated",&m.gaps_truncated},{"prehistory_short",&m.prehistory_short},{"tail_missing",&m.tail_missing}};
 for(unsigned k=0;k<sizeof(booleans)/sizeof(booleans[0]);k++)if(!metadata_json_bool(d,FIELD(d,0,booleans[k].name),booleans[k].v))return false;
 if(!identifier(d,FIELD(d,0,"close_reason"),m.close_reason,32)||!identifier(d,FIELD(d,0,"missing_ref_reason"),m.missing_ref_reason,32)||!identifier(d,FIELD(d,0,"evidence_reason"),m.evidence_reason,32))return false;
 unsigned state;for(state=0;state<4;state++)if(text(d,FIELD(d,0,"state"),event_states[state]))break;if(state==4)return false;m.state=(StorageEventState)state;
 int prev=FIELD(d,0,"prev_meta");if(prev<0||d->token[prev].type!=MJ_OBJECT||!u64(d,prev,"file_id",&m.prev_meta.file_id)||!u64(d,prev,"record_seq",&m.prev_meta.record_seq))return false;
 int triggers=FIELD(d,0,"triggers"),pcm=FIELD(d,0,"pcm_refs"),gaps=FIELD(d,0,"gaps");
 if(triggers<0||pcm<0||gaps<0||d->token[triggers].type!=MJ_ARRAY||d->token[pcm].type!=MJ_ARRAY||d->token[gaps].type!=MJ_ARRAY||d->token[triggers].count>32||d->token[pcm].count>32||d->token[gaps].count>32)return false;
 m.trigger_count=d->token[triggers].count;m.pcm_count=d->token[pcm].count;m.gap_count=d->token[gaps].count;
 for(unsigned k=0;k<m.trigger_count;k++)if(!metadata_json_u64(d,metadata_json_index(d,triggers,k),&m.triggers[k]))return false;
 for(unsigned k=0;k<m.pcm_count;k++){
  int item=metadata_json_index(d,pcm,k);StorageEventPcmRef *a=&m.pcm[k];if(item<0||d->token[item].type!=MJ_OBJECT||!u64(d,item,"file_id",&a->file_id)||!u64(d,item,"record_seq",&a->record_seq)||!u64(d,item,"first_raw",&a->first_raw)||!u64(d,item,"end_raw",&a->end_raw)||!u32(d,item,"raw_stride",&a->raw_stride)||!u32(d,item,"sample_count",&a->sample_count))return false;
 }
 for(unsigned k=0;k<m.gap_count;k++){int item=metadata_json_index(d,gaps,k);if(item<0||d->token[item].type!=MJ_ARRAY||d->token[item].count!=2||!metadata_json_u64(d,metadata_json_index(d,item,0),&m.gaps[k].first_raw)||!metadata_json_u64(d,metadata_json_index(d,item,1),&m.gaps[k].end_raw))return false;}
 if(m.session_id!=id->session_id||m.capture_id!=id->capture_id||m.config_id!=id->config_id||m.actual_first!=id->first_raw||m.end_raw!=id->end_raw||!event_consistent(&m))return false;
 *out=m;return true;
}
static bool event_add(uint8_t *b,size_t cap,size_t *used,const char *fmt,...){
 if(*used>=cap)return false;
 va_list args;va_start(args,fmt);int n=vsnprintf((char*)b+*used,cap-*used,fmt,args);va_end(args);
 if(n<0||(size_t)n>=cap-*used)return false;
 *used+=(size_t)n;return true;
}
bool storage_event_record(const StorageEventMeta*m,StorageRequest*out,uint8_t*b,size_t cap){
 if(!out||!b||!event_consistent(m))return false;
 size_t n=0;
#define EA(...) do{if(!event_add(b,cap,&n,__VA_ARGS__))return false;}while(0)
#define EU(name) EA(",\"" #name "\":\"%llu\"",(unsigned long long)m->name)
#define EB(name) EA(",\"" #name "\":%s",m->name?"true":"false")
 EA("{\"schema_version\":1,\"device_id\":\"%s\",\"config_id\":%u,\"segment_no\":%u,\"state\":\"%s\"",m->device_id,m->config_id,m->segment_no,event_states[m->state]);
 EU(session_id);EU(capture_id);EU(event_id);EU(persist_revision);EU(onset_raw);EU(prehistory_requested_first);EU(actual_first);EU(end_raw);EU(append_cursor);EU(trigger_total);EU(valid_sample_count);EU(evidence_count);EU(evidence_first_raw);EU(evidence_end_raw);
 EB(event_closed);EB(triggers_truncated);EB(gaps_truncated);EB(prehistory_short);EB(tail_missing);
 EA(",\"close_reason\":\"%s\",\"missing_ref_reason\":\"%s\",\"evidence_reason\":\"%s\",\"prev_meta\":{\"file_id\":\"%llu\",\"record_seq\":\"%llu\"},\"triggers\":[",m->close_reason,m->missing_ref_reason,m->evidence_reason,(unsigned long long)m->prev_meta.file_id,(unsigned long long)m->prev_meta.record_seq);
 for(unsigned k=0;k<m->trigger_count;k++)EA("%s\"%llu\"",k?",":"",(unsigned long long)m->triggers[k]);
 EA("],\"pcm_refs\":[");for(unsigned k=0;k<m->pcm_count;k++){const StorageEventPcmRef*a=&m->pcm[k];EA("%s{\"file_id\":\"%llu\",\"record_seq\":\"%llu\",\"first_raw\":\"%llu\",\"end_raw\":\"%llu\",\"raw_stride\":%u,\"sample_count\":%u}",k?",":"",(unsigned long long)a->file_id,(unsigned long long)a->record_seq,(unsigned long long)a->first_raw,(unsigned long long)a->end_raw,a->raw_stride,a->sample_count);}
 EA("],\"gaps\":[");for(unsigned k=0;k<m->gap_count;k++)EA("%s[\"%llu\",\"%llu\"]",k?",":"",(unsigned long long)m->gaps[k].first_raw,(unsigned long long)m->gaps[k].end_raw);EA("]}");
#undef EA
#undef EU
#undef EB
 memset(out,0,sizeof(*out));out->identity=(RecordIdentity){RECORD_EVENT_META,0,m->config_id,m->session_id,m->capture_id,m->actual_first,m->end_raw,0};out->payload=b;out->payload_bytes=n;return true;
}
