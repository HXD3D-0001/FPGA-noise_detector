#include "noise_report.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct { char *b; size_t cap,used; bool ok; } Writer;
static void put(Writer *w,const char *fmt,...){
  if(!w->ok)return;
  va_list a;va_start(a,fmt);
  int n=vsnprintf(w->b+w->used,w->used<w->cap?w->cap-w->used:0,fmt,a);
  va_end(a);
  if(n<0||(size_t)n>=(w->used<w->cap?w->cap-w->used:0)){w->ok=false;return;}
  w->used+=(size_t)n;
}
static void put_db(Writer *w,bool present,double value){
  if(present&&isfinite(value))put(w,"%.2f",value);else put(w,"null");
}
static void put_text_db(Writer *w,bool present,double value){
  if(present&&isfinite(value))put(w,"%.2f",value);else put(w,"-");
}
static void put_band_level(Writer *w,const LfnReportData *d,const LfnBandLevel *b,bool json){
  if(b->below_floor||b->power_linear<=0.0||!isfinite(b->power_linear)){
    put(w,"%s",json?"null":"-");
    return;
  }
  double dbfs=10.0*log10(b->power_linear);
  put(w,"%.2f",dbfs);
  /* Apply the same measured gain to each band of this input. */
  if(d->source==LFN_REPORT_SOURCE_ADC&&d->adc_left&&d->live->valid_ns)
    put(w,json?",\"estimated_spl_db\":%.1f":" estimated_spl_db=%.1f",dbfs+120.125);
}
static void json_string(Writer *w,const char *s,size_t n){
  put(w,"\"");
  for(size_t i=0;i<n;i++){
    unsigned char c=(unsigned char)s[i];
    if(c=='\"'||c=='\\')put(w,"\\%c",c);
    else if(c=='\b')put(w,"\\b");
    else if(c=='\f')put(w,"\\f");
    else if(c=='\n')put(w,"\\n");
    else if(c=='\r')put(w,"\\r");
    else if(c=='\t')put(w,"\\t");
    else if(c<0x20)put(w,"\\u%04x",(unsigned)c);
    else put(w,"%c",(char)c);
  }
  put(w,"\"");
}
static void text_string(Writer *w,const char *s,size_t n){
  put(w,"\"");
  for(size_t i=0;i<n;i++){
    char c=s[i];
    if(c=='\"'||c=='\\')put(w,"\\%c",c);
    else if((unsigned char)c<0x20)put(w," ");
    else put(w,"%c",c);
  }
  put(w,"\"");
}
static const char *status_name(LfnCompareStatus s){
  switch(s){case LFN_COMPARE_OK:return "OK";case LFN_COMPARE_INCOMPATIBLE:return "INCOMPATIBLE";
            case LFN_COMPARE_INVALID:return "INVALID";}
  return "INVALID";
}
static const char *source_name(LfnReportSource source){
  return source==LFN_REPORT_SOURCE_ADC?"ADC":source==LFN_REPORT_SOURCE_TEST?"TEST":NULL;
}
bool lfn_report_json(const LfnReportData *d,char *buffer,size_t capacity){
  if(!d||!buffer||!capacity||(d->marker_count&&!d->markers)||
     (d->has_comparison&&!d->comparison))return false;
  Writer w={buffer,capacity,0,true};
  put(&w,"{\"schema_version\":1,\"kind\":\"noise_report\",\"session_id\":\"%llu\","
        "\"capture_id\":\"%llu\",\"config_id\":%u,",
      (unsigned long long)d->session_id,(unsigned long long)d->capture_id,d->config_id);
  const char *source=source_name(d->source);
  if(source)put(&w,"\"source_mode\":\"%s\",",source);else put(&w,"\"source_mode\":null,");
  if(d->has_start_utc)put(&w,"\"start_utc_ns\":\"%lld\",",(long long)d->start_utc_ns);
  else put(&w,"\"start_utc_ns\":null,");
  put(&w,"\"start_uncertainty_ns\":%llu,\"duration_ns\":%llu,\"valid_ns\":%llu,"
        "\"expected_ns\":%llu,",
      (unsigned long long)d->start_uncertainty_ns,(unsigned long long)d->duration_ns,
      (unsigned long long)d->valid_ns,(unsigned long long)d->expected_ns);
  if(d->persisted)put(&w,"\"data_source\":\"SD_RECORDS\",\"dropped_frames\":null,\"clipped_samples\":null,\"invalid_or_missing_frames\":%llu,",(unsigned long long)d->dropped_frames);
  else put(&w,"\"dropped_frames\":%llu,\"clipped_samples\":%llu,",(unsigned long long)d->dropped_frames,(unsigned long long)d->clipped_samples);
  put(&w,"\"markers\":[");
  for(unsigned i=0;i<d->marker_count;i++){
    const LfnMarker *m=&d->markers[i];
    if(i)put(&w,",");
    put(&w,"{\"marker_id\":%llu,\"strength\":%u,\"start_raw\":%llu,",
        (unsigned long long)m->marker_id,m->strength,(unsigned long long)m->start.raw_index);
    if(m->has_end)put(&w,"\"end_raw\":%llu,",(unsigned long long)m->end.raw_index);
    else put(&w,"\"end_raw\":null,");
    put(&w,"\"config_id\":%u,\"sync_map_id\":%llu,\"uncertainty_ns\":%llu,",
        m->start.config_id,(unsigned long long)m->sync_map_id,(unsigned long long)m->uncertainty_ns);
    if(m->has_client_utc)put(&w,"\"client_utc_ns\":\"%lld\",",(long long)m->client_utc_ns);
    else put(&w,"\"client_utc_ns\":null,");
    put(&w,"\"note\":");
    json_string(&w,m->note,strlen(m->note));
    put(&w,"}");
  }
  put(&w,"]");
  if(d->live){
    put(&w,",\"%s\":{\"duration_ns\":%llu,\"valid_ns\":%llu,\"bands\":[",d->persisted?"stored":"live",
        (unsigned long long)d->live->duration_ns,(unsigned long long)d->live->valid_ns);
    for(unsigned i=0;i<d->live->band_count;i++){
      const LfnBandLevel *b=&d->live->bands[i];
      if(i)put(&w,",");
      put(&w,"{\"low_hz\":%.3f,\"high_hz\":%.3f,\"dbfs\":",b->low_hz,b->high_hz);
      put_band_level(&w,d,b,true);
      put(&w,",\"below_floor\":%s}",b->below_floor?"true":"false");
    }
    put(&w,"]}");
  }
  if(d->has_comparison){
    const LfnCompareResult *c=d->comparison;
    put(&w,",\"comparison\":{");
    if(d->persisted)put(&w,"\"basis\":\"RELATIVE_FS_POWER\",\"physical_conditions_confirmed\":false,");
    put(&w,"\"status\":\"%s\",\"absolute_allowed\":%s,"
          "\"mismatch_mask\":%u,\"bands\":[",
        status_name(c->status),c->absolute_allowed?"true":"false",c->mismatch_mask);
    for(unsigned i=0;i<c->band_count;i++){
      const LfnBandComparison *b=&c->bands[i];
      double low=NAN,high=NAN;
      if(d->before&&i<d->before->band_count){low=d->before->bands[i].low_hz;high=d->before->bands[i].high_hz;}
      if(i)put(&w,",");
      put(&w,"{\"low_hz\":%.3f,\"high_hz\":%.3f,",low,high);
      put(&w,"\"before_db\":");put_db(&w,!b->before_null,b->before_db);
      put(&w,",\"after_db\":");put_db(&w,!b->after_null,b->after_db);
      put(&w,",\"delta_db\":");put_db(&w,b->delta_valid,b->delta_db);
      put(&w,",\"below_floor\":%s}",b->below_floor?"true":"false");
    }
    put(&w,"]}");
  }
  put(&w,"}\n");
  return w.ok;
}
bool lfn_report_text(const LfnReportData *d,char *buffer,size_t capacity){
  if(!d||!buffer||!capacity||(d->marker_count&&!d->markers)||
     (d->has_comparison&&!d->comparison))return false;
  Writer w={buffer,capacity,0,true};
  put(&w,"LFN NOISE REPORT v1\n");
  put(&w,"session=%llu capture=%llu config=%u\n",(unsigned long long)d->session_id,
      (unsigned long long)d->capture_id,d->config_id);
  const char *source=source_name(d->source);put(&w,"source_mode=%s\n",source?source:"UNKNOWN");
  if(d->has_start_utc)put(&w,"start_utc_ns=%lld uncertainty_ns=%llu\n",(long long)d->start_utc_ns,
      (unsigned long long)d->start_uncertainty_ns);
  else put(&w,"start_utc_ns=- uncertainty_ns=%llu\n",(unsigned long long)d->start_uncertainty_ns);
  put(&w,"duration_ns=%llu valid_ns=%llu expected_ns=%llu ",
      (unsigned long long)d->duration_ns,(unsigned long long)d->valid_ns,
      (unsigned long long)d->expected_ns);
  if(d->persisted)put(&w,"invalid_or_missing_frames=%llu dropped_frames=- clipped_samples=-\n",(unsigned long long)d->dropped_frames);
  else put(&w,"dropped_frames=%llu clipped_samples=%llu\n",(unsigned long long)d->dropped_frames,(unsigned long long)d->clipped_samples);
  put(&w,"markers=%u\n",d->marker_count);
  for(unsigned i=0;i<d->marker_count;i++){
    const LfnMarker *m=&d->markers[i];
    put(&w,"marker id=%llu strength=%u start_raw=%llu end_raw=",
        (unsigned long long)m->marker_id,m->strength,(unsigned long long)m->start.raw_index);
    if(m->has_end)put(&w,"%llu",(unsigned long long)m->end.raw_index);else put(&w,"-");
    put(&w," note=");text_string(&w,m->note,strlen(m->note));put(&w,"\n");
  }
  if(d->live){
    put(&w,"%s duration_ns=%llu valid_ns=%llu\n",d->persisted?"stored":"live",
        (unsigned long long)d->live->duration_ns,(unsigned long long)d->live->valid_ns);
    for(unsigned i=0;i<d->live->band_count;i++){
      const LfnBandLevel *b=&d->live->bands[i];
      put(&w,"%s_band[%u] low_hz=%.3f high_hz=%.3f dbfs=",d->persisted?"stored":"live",i,b->low_hz,b->high_hz);
      put_band_level(&w,d,b,false);
      put(&w," below_floor=%s\n",b->below_floor?"true":"false");
    }
  }
  if(d->has_comparison){
    const LfnCompareResult *c=d->comparison;
    if(d->persisted)put(&w,"comparison basis=RELATIVE_FS_POWER physical_conditions_confirmed=false\n");
    put(&w,"comparison status=%s absolute_allowed=%s mismatch_mask=0x%x\n",
        status_name(c->status),c->absolute_allowed?"true":"false",c->mismatch_mask);
    for(unsigned i=0;i<c->band_count;i++){
      const LfnBandComparison *b=&c->bands[i];
      double low=NAN,high=NAN;
      if(d->before&&i<d->before->band_count){low=d->before->bands[i].low_hz;high=d->before->bands[i].high_hz;}
      put(&w,"band[%u] low_hz=%.3f high_hz=%.3f before_db=",i,low,high);
      put_text_db(&w,!b->before_null,b->before_db);
      put(&w," after_db=");put_text_db(&w,!b->after_null,b->after_db);
      put(&w," delta_db=");put_text_db(&w,b->delta_valid,b->delta_db);
      put(&w," below_floor=%s\n",b->below_floor?"true":"false");
    }
  }
  return w.ok;
}
