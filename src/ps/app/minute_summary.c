#include "minute_summary.h"
#include "capture_time_contract.h"
#include <string.h>
#include <math.h>
bool summary_scale_floor(uint64_t a,uint64_t b,uint64_t d,uint64_t *out){
 if(!d||a>d||!out)return false;
 uint64_t q=0,r=0;
 for(unsigned k=64;k>0;k--){q*=2;
 if(r>=d-r){r-=d-r;q++;}else r+=r;
 if((b>>(k-1))&1){if(r>=d-a){r-=d-a;q++;}else r+=a;}}
 *out=q;return true;
}
static bool text(const char *s){if(!s[0]||!memchr(s,0,64))return false;
 for(unsigned k=0;k<64&&s[k];k++)if((unsigned char)s[k]<33||(unsigned char)s[k]>126||s[k]=='"'||s[k]=='\\')return false;
 return true;}
bool minute_summary_init(MinuteSummaryService *s,const SummaryIdentity *id,uint64_t period,uint64_t first,uint64_t seed){
 if(!s||!id||!id->session_id||!id->capture_id||!id->config_id||!text(id->device_id)||!text(id->band_definition_id)||!period||!seed||seed==UINT64_MAX)return false;
 uint64_t begin=first-first%period;
 if(begin>UINT64_MAX-period)return false;
 SummaryIdentity copy=*id;memset(s,0,sizeof(*s));s->period_ns=period;s->cursor=first;s->next_id=seed;s->current.identity=copy;s->current.first_ns=begin;s->current.end_ns=begin+period;s->current.observed_first_ns=first;s->current.summary_id=seed;s->current.revision=1;s->current.partial=first!=begin;return true;
}
bool minute_summary_offer(MinuteSummaryService *s,const SummaryInterval *i){
 SummaryInterval mapped;
 if(s&&i&&s->current.schema_version==2){
  const MinuteSummary *m=&s->current;
  if(!i->raw_known||i->first_raw<s->last_raw_end||(i->first_raw-s->last_raw_end)%48||i->end_raw<=i->first_raw||(i->end_raw-i->first_raw)%48||i->expected_candidates>1||i->valid_candidates>i->expected_candidates||(!i->valid&&i->valid_candidates)||(!i->availability_known&&i->available)||(i->valid&&(!i->statistic_known||!i->availability_known))||(i->pcm_saved&&!i->pcm_valid))return false;
  mapped=*i;
  if(!capture_time_pair_map_ns(&m->time_pair,i->first_raw,&mapped.first_ns)||!capture_time_pair_map_ns(&m->time_pair,i->end_raw,&mapped.end_ns))return false;
  mapped.expected_frames=mapped.valid_frames=0;i=&mapped;
 }
 if(!s||!i||s->fault||s->closed||s->ready||s->have_input||i->first_ns!=s->cursor||i->end_ns<=i->first_ns||(i->available&&!i->requested)||(i->valid&&!i->available)||(i->saved&&!i->valid)||(i->pcm_valid&&!i->available)||(i->raw_known&&i->end_raw<=i->first_raw)||(!i->raw_known&&(i->valid||i->pcm_valid))||!isfinite(i->expected_frames)||!isfinite(i->valid_frames)||i->expected_frames<0||i->valid_frames<0||i->valid_frames>i->expected_frames||(!i->valid&&i->valid_frames))return false;
 if((!i->raw_known&&(i->first_raw||i->end_raw))||(i->raw_known&&s->have_raw&&i->first_raw<s->last_raw_end))return false;
 if(i->valid)for(unsigned k=0;k<SUMMARY_BANDS;k++)if(!isfinite(i->power[k])||i->power[k]<0)return false;
 s->input=*i;s->input_cursor=i->first_ns;s->have_input=true;return true;
}
void minute_summary_step(MinuteSummaryService *s){
 if(!s||s->fault||s->ready||!s->have_input)return;
 MinuteSummary *m=&s->current;SummaryInterval *i=&s->input;uint64_t end=i->end_ns<m->end_ns?i->end_ns:m->end_ns,dt=end-s->input_cursor,span=i->end_ns-i->first_ns;
 bool v2=m->schema_version==2;double fraction=(double)dt/(double)span,ef=i->expected_frames*fraction,vf=i->valid_frames*fraction,add[SUMMARY_BANDS]={0};unsigned bins[SUMMARY_BANDS]={0};uint64_t raw_first=0,raw_end=0,ticks=0;
 if(i->raw_known){uint64_t a=0,b=0;
 if(!summary_scale_floor(s->input_cursor-i->first_ns,i->end_raw-i->first_raw,span,&a)||!summary_scale_floor(end-i->first_ns,i->end_raw-i->first_raw,span,&b)){s->fault=true;return;}raw_first=i->first_raw+a;raw_end=i->first_raw+b;ticks=b-a;}
 uint64_t ec=v2&&s->input_cursor==i->first_ns?i->expected_candidates:0,vc=v2&&s->input_cursor==i->first_ns?i->valid_candidates:0;
 if((v2&&(dt%1000000||ticks%48||ec>UINT64_MAX-m->expected_candidates||vc>UINT64_MAX-m->valid_candidates))||!isfinite(m->expected_frames+ef)||!isfinite(m->valid_frames+vf)||(i->requested&&dt>UINT64_MAX-m->requested_ns)||(i->available&&dt>UINT64_MAX-m->available_ns)||(i->valid&&dt>UINT64_MAX-m->valid_ns)||(i->requested&&!i->available&&dt>UINT64_MAX-m->unavailable_ns)||(i->valid&&!i->saved&&dt>UINT64_MAX-m->unsaved_ns)||(i->pcm_valid&&ticks>UINT64_MAX-m->pcm_valid_ticks)){s->fault=true;return;}
 if(i->valid)for(unsigned k=0;k<SUMMARY_BANDS;k++){
  add[k]=i->power[k]*(v2?(double)dt/1e9:(double)dt);
 if(!isfinite(add[k])||!isfinite(m->sum_power_weighted[k]+add[k])){s->fault=true;return;}
  double db=i->power[k]>0?10*(log10(i->power[k])-(v2?log10(0.5):0)):-INFINITY;bins[k]=db< -120?0:db>=0?25:(unsigned)floor((db+120)/5)+1;
  if(dt>UINT64_MAX-m->histogram_ns[k][bins[k]]){s->fault=true;return;}
 }
 if(i->requested)m->requested_ns+=dt;
 if(i->available)m->available_ns+=dt;
 if(i->valid)m->valid_ns+=dt;
 if(i->requested&&!i->available&&(!v2||i->availability_known))m->unavailable_ns+=dt;
 if(i->valid&&!i->saved)m->unsaved_ns+=dt;
 if(i->pcm_valid)m->pcm_valid_ticks+=ticks;
 if(v2){m->expected_candidates+=ec;m->valid_candidates+=vc;if(i->pcm_saved)m->pcm_saved_ticks+=ticks;if(i->requested&&!i->statistic_known)m->statistic_unknown_ns+=dt;if(i->requested&&!i->availability_known)m->source_available_unknown_ns+=dt;}
 m->expected_frames+=ef;m->valid_frames+=vf;m->flags|=i->flags;
 if(i->raw_known){if(!m->raw_known)m->first_raw=raw_first;m->end_raw=raw_end;m->raw_known=true;}
 if(i->valid)for(unsigned k=0;k<SUMMARY_BANDS;k++){m->sum_power_weighted[k]+=add[k];m->histogram_ns[k][bins[k]]+=dt;}
 s->input_cursor=end;s->cursor=end;
 if(end==i->end_ns){s->have_input=false;
 if(i->raw_known){s->have_raw=true;s->last_raw_end=i->end_raw;}}if(end==m->end_ns)s->ready=true;
}
bool minute_summary_take(MinuteSummaryService *s,MinuteSummary *out){
 if(!s||!out||!s->ready||s->fault)return false;
 *out=s->current;s->ready=false;
 if(s->closed)return true;
 if(s->current.schema_version==2){
  MinuteSummary old=s->current;uint64_t bucket=old.bucket_end_ns;
  if(bucket>UINT64_MAX-s->period_ns||old.end_ns>UINT64_MAX-s->period_ns||s->next_id==UINT64_MAX-1){s->fault=true;return true;}
  memset(&s->current,0,sizeof(s->current));MinuteSummary *m=&s->current;
  m->identity=old.identity;m->schema_version=2;m->capture_revision=old.capture_revision;m->session_start_ns=old.session_start_ns;m->time_pair=old.time_pair;
  m->bucket_start_ns=bucket;m->bucket_end_ns=bucket+s->period_ns;m->first_ns=m->observed_first_ns=old.end_ns;m->end_ns=old.end_ns+s->period_ns;
  m->quantization_start_ns=old.quantization_end_ns;m->quantization_end_ns=old.quantization_end_ns;m->summary_id=++s->next_id;m->revision=1;return true;
 }
 uint64_t end=s->current.end_ns;
 if(end>UINT64_MAX-s->period_ns||s->next_id==UINT64_MAX-1){s->fault=true;return true;}
 SummaryIdentity id=s->current.identity;memset(&s->current,0,sizeof(s->current));s->current.identity=id;s->current.first_ns=end;s->current.end_ns=end+s->period_ns;s->current.observed_first_ns=end;s->current.summary_id=++s->next_id;s->current.revision=1;return true;
}
bool minute_summary_init_v2(MinuteSummaryService *s,const SummaryIdentity *id,const CaptureTimePair *p,uint64_t session,uint64_t raw,uint64_t revision,uint64_t period,uint64_t seed){
 uint64_t first;
 if(!s||!id||!revision||!capture_time_nominal_valid(p)||id->test_source!=(p->source==PL_CAPTURE_TEST)||p->session_id!=id->session_id||p->capture_id!=id->capture_id||p->config_id!=id->config_id||!period||period%1000000||!capture_time_pair_map_ns(p,raw,&first)||first<session)return false;
 uint64_t bucket=first-(first-session)%period;
 if(bucket>UINT64_MAX-period)return false;
 uint64_t offset=(first-bucket)%1000000,end=bucket+period;
 if(end>UINT64_MAX-offset)return false;
 MinuteSummaryService copy;
 if(!minute_summary_init(&copy,id,period,first,seed))return false;
 MinuteSummary *m=&copy.current;m->schema_version=2;m->capture_revision=revision;m->session_start_ns=session;m->time_pair=*p;
 m->bucket_start_ns=bucket;m->bucket_end_ns=end;m->first_ns=m->observed_first_ns=first;m->end_ns=end+offset;
 m->quantization_start_ns=m->quantization_end_ns=offset;m->partial=first!=bucket+offset;copy.last_raw_end=raw;
 *s=copy;return true;
}
bool minute_summary_flush(MinuteSummaryService *s){if(!s||s->fault||s->closed||s->ready||s->have_input||s->cursor==s->current.observed_first_ns)return false;
 s->current.end_ns=s->cursor;s->current.partial=true;s->closed=true;s->ready=true;return true;}
bool minute_summary_reference(MinuteSummaryService *s,bool marker,uint64_t id){
 if(!s||!id||s->fault||s->closed||s->ready)return false;
 MinuteSummary *m=&s->current;uint64_t *refs=marker?m->markers:m->events,*total=marker?&m->marker_total:&m->event_total;unsigned *count=marker?&m->marker_count:&m->event_count;
 for(unsigned k=0;k<*count;k++)if(refs[k]==id)return true;
 if(*total==UINT64_MAX)return false;
 (*total)++;
 if(*count<SUMMARY_REFERENCES)refs[(*count)++]=id;else m->references_truncated=true;return true;
}
