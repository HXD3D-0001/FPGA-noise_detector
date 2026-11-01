#include "network_snapshot.h"
#include "capture_image.h"
#include "storage_runtime.h"
#include "noise_report.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* These copies are too large for the network task stack. Never expose producer
 * pointers or retain an earlier snapshot when the current copy fails. */
static LfnServiceSnapshot view;
static LfnMarkerStore markers;
static LfnLevelSet levels;
static StorageRuntimeHealth storage;
static const char *source_json(bool have){
 if(!have)return "null";
 const PlCaptureConfig *c=view.capture_valid?&view.active.config:
                          view.prepared_valid?&view.prepared.config:
                          view.requested_session?&view.requested:NULL;
 return !c?"null":c->source==PL_CAPTURE_TEST?"\"TEST\"":c->source==PL_CAPTURE_ADC?"\"ADC\"":"null";
}
static bool complete(char *out,size_t cap,int n){
 if(n<0||(size_t)n>=cap){out[0]=0;return false;}
 return true;
}
bool lfn_network_image_json(const char *path,char *out,size_t cap){
 if(!out||!cap)return false;
 out[0]=0;
 if(!path||(strcmp(path,"/api/status")&&strcmp(path,"/api/spectrum")))return false;
 bool have=lfn_capture_image_snapshot_copy(&view)==SERVICE_OK;
 const char *source=source_json(have);
 if(!strcmp(path,"/api/status")){
  int n;
  if(have)n=snprintf(out,cap,"{\"have_snapshot\":true,\"capture\":{\"state\":%u,\"runtime_state\":%u,\"failure\":%ld,\"source_mode\":%s},",(unsigned)view.application_state,(unsigned)view.runtime_state,(long)view.failure,source);
  else n=snprintf(out,cap,"{\"have_snapshot\":false,\"capture\":{\"state\":null,\"runtime_state\":null,\"failure\":null,\"source_mode\":null},");
  if(!complete(out,cap,n))return false;
  size_t used=(size_t)n;
  if(lfn_storage_runtime_health(&storage)){
   const MarkerRuntimeHealth *m=&storage.diagnostics.markers;
   n=snprintf(out+used,cap-used,"\"storage\":{\"active\":%s,\"closed\":%s,\"failed\":%s,\"session\":\"%llu\",\"markers\":{\"queued\":%llu,\"synced\":%llu,\"failed\":%llu,\"pending\":%u}}}",storage.active?"true":"false",storage.closed?"true":"false",storage.failed?"true":"false",(unsigned long long)storage.session,(unsigned long long)m->queued,(unsigned long long)m->synced,(unsigned long long)m->failed,m->pending);
  }else n=snprintf(out+used,cap-used,"\"storage\":null}");
  if(!complete(out+used,cap-used,n)){out[0]=0;return false;}
  return true;
 }
 const LfnSpectrum *sp=&view.analysis.spectrum;
 bool valid=have&&view.capture_valid&&view.analysis.has_spectrum&&sp->valid&&
  sp->session_id&&sp->capture_id&&sp->config_id&&
  sp->session_id==view.active.session_id&&sp->capture_id==view.active.capture_id&&
  sp->config_id==view.active.config.config_id&&
  (view.active.config.source==PL_CAPTURE_TEST||view.active.config.source==PL_CAPTURE_ADC)&&
  sp->test_source==(view.active.config.source==PL_CAPTURE_TEST);
 if(!valid)return complete(out,cap,snprintf(out,cap,"{\"status\":\"NO_SPECTRUM\",\"live\":null,\"have_snapshot\":%s,\"data_source\":\"LIVE_SNAPSHOT\",\"source_mode\":%s}",have?"true":"false",source));
 memset(&levels,0,sizeof levels);
 levels.band_count=LFN_SPECTRUM_BANDS;
 for(unsigned i=0;i<LFN_SPECTRUM_BANDS;i++){
  const LfnSpectrumBand *b=&sp->bands[i];
  levels.bands[i]=(LfnBandLevel){b->low_hz,b->high_hz,b->power,b->level.status!=LFN_LEVEL_VALID};
 }
 if(!isfinite(sp->observation_seconds)||sp->observation_seconds<0||sp->observation_seconds>=18446744073.0)return false;
 levels.duration_ns=(uint64_t)(sp->observation_seconds*1e9);
 levels.valid_ns=levels.expected_ns=levels.duration_ns;
 bool have_markers=lfn_capture_image_markers_copy(&markers);
 unsigned count=0;
 if(have_markers){
  for(unsigned i=0;i<markers.count;i++){
   const LfnMarker *m=&markers.items[i];
   if(m->start.session_id==sp->session_id&&m->start.capture_id==sp->capture_id&&m->start.config_id==sp->config_id)markers.items[count++]=*m;
  }
 }
 LfnReportData report={0};
 report.session_id=sp->session_id;report.capture_id=sp->capture_id;report.config_id=sp->config_id;
 report.source=sp->test_source?LFN_REPORT_SOURCE_TEST:LFN_REPORT_SOURCE_ADC;
 report.adc_left=view.active.config.channel==0;
 report.duration_ns=levels.duration_ns;report.valid_ns=levels.valid_ns;report.expected_ns=levels.expected_ns;
 report.markers=markers.items;report.marker_count=count;report.live=&levels;
 report.dropped_frames=view.analysis.health.missing_frames;
 if(!lfn_report_json(&report,out,cap)){out[0]=0;return false;}
 size_t used=strlen(out)-2; /* renderer terminates with }\n */
 int n=snprintf(out+used,cap-used,",\"data_source\":\"LIVE_SNAPSHOT\",\"frame_seq\":\"%llu\",\"sample_raw\":\"%llu\",\"published_us\":\"%llu\",\"markers_available\":%s}",(unsigned long long)sp->stream_seq,(unsigned long long)sp->end_raw,(unsigned long long)view.published_us,have_markers?"true":"false");
 if(!complete(out+used,cap-used,n)){out[0]=0;return false;}
 return true;
}
