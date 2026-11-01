#include "event_engine.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#define HOP UINT64_C(6144)
#define WINDOW UINT64_C(12288)
#define SECOND UINT64_C(48000)
#define BACKGROUND (60*SECOND)
#define LATE UINT64_C(589824)
static EventResult exhausted(EventEngine *e){e->view.exhausted=true;return EVENT_EXHAUSTED;}
static bool increment(EventEngine *e,uint64_t *v){if(*v==UINT64_MAX){exhausted(e);return false;}++*v;return true;}
static bool emit(EventEngine *e,EventChangeKind kind,const EventRecord *r){
 if(e->output_count==EVENT_OUTPUT_SLOTS){exhausted(e);return false;}
 EventChange *c=&e->outputs[(e->output_head+e->output_count++)%EVENT_OUTPUT_SLOTS];c->kind=kind;c->event=*r;return true;
}
static bool same(const LfnServiceIdentity *a,const LfnServiceIdentity *b){
 return a->session_id==b->session_id&&a->capture_id==b->capture_id&&a->config.config_id==b->config.config_id&&a->config.source==b->config.source&&a->config.mode==b->config.mode&&a->config.channel==b->config.channel&&a->config.pattern==b->config.pattern;
}
static bool clock_update(EventEngine *e,uint64_t now){
 if(e->have_clock&&now<e->now)return false;
 e->have_clock=true;e->now=now;
 for(unsigned i=0;i<e->pending_count;){
  if(now-e->pending[i].closed_at>LATE){
   increment(e,&e->view.expired_pending);e->pending[i]=e->pending[--e->pending_count];
  }else ++i;
 }return true;
}
static bool active(const EventEngine *e){return e->view.state==EVENT_ACTIVE||e->view.state==EVENT_TAIL;}
static bool close_event(EventEngine *e,EventCloseReason reason){
 EventRecord *r=&e->view.current;r->closed=true;r->reason=reason;r->end_raw=e->last_end;
 if(!increment(e,&r->revision))return false;
 if(!emit(e,EVENT_CLOSE,r))return false;
 if(e->pending_count==EVENT_PENDING_SLOTS){increment(e,&e->view.pending_overflow);return true;}
 e->pending[e->pending_count].record=*r;e->pending[e->pending_count++].closed_at=e->now;return true;
}
static void reset_background(EventEngine *e){e->bg_count=e->bg_head=0;e->high_count=e->low_count=0;e->have_energy=false;e->view.state=EVENT_WARMUP;e->view.background_ticks=0;e->view.background_power=e->floor_power;}
static void discontinuity(EventEngine *e,EventCloseReason reason){
 if(active(e))close_event(e,reason);
 increment(e,&e->view.discontinuities);reset_background(e);
}
static int compare(const void *a,const void *b){double x=*(const double*)a,y=*(const double*)b;return (x>y)-(x<y);}
static bool background(EventEngine *e,uint64_t end){
 bool had_background=e->bg_count!=0;
 uint64_t left=end>BACKGROUND?end-BACKGROUND:0,coverage=0;double values[EVENT_BACKGROUND_SLOTS];unsigned n=0;
 while(e->bg_count&&(e->background[e->bg_head].end<=left||e->now-e->background[e->bg_head].at>BACKGROUND)){e->bg_head=(e->bg_head+1)%EVENT_BACKGROUND_SLOTS;e->bg_count--;}
 for(unsigned j=0;j<e->bg_count;j++){
  unsigned i=(e->bg_head+j)%EVENT_BACKGROUND_SLOTS;uint64_t t=e->background[i].end;
  coverage+=t-(t-HOP>left?t-HOP:left);values[n++]=e->background[i].power;
 }
 if(!n&&had_background)e->start_raw=end-HOP;
 e->view.background_ticks=coverage;
 if(!n||end-e->start_raw<BACKGROUND||coverage<57*SECOND)return false;
 uint64_t last=e->background[(e->bg_head+e->bg_count-1)%EVENT_BACKGROUND_SLOTS].end;
 if(end-last>BACKGROUND)return false;
 qsort(values,n,sizeof(values[0]),compare);double p=n%2?values[n/2]:(values[n/2-1]+values[n/2])/2;
 e->view.background_power=p>e->floor_power?p:e->floor_power;return true;
}
static void add_background(EventEngine *e,uint64_t end,double power){
 if(e->bg_count==EVENT_BACKGROUND_SLOTS){e->bg_head=(e->bg_head+1)%EVENT_BACKGROUND_SLOTS;e->bg_count--;}
 unsigned i=(e->bg_head+e->bg_count++)%EVENT_BACKGROUND_SLOTS;e->background[i].end=end;e->background[i].at=e->now;e->background[i].power=power;
}
bool event_engine_init(EventEngine *e,double floor){
 if(!e||!isfinite(floor)||floor<=0||floor>1)return false;
 memset(e,0,sizeof(*e));e->initialized=true;e->floor_power=floor;e->view.background_power=floor;e->next_id=1;return true;
}
bool event_engine_snapshot(const EventEngine *e,EventSnapshot *s){if(!e||!s||!e->initialized)return false;*s=e->view;return true;}
bool event_engine_take(EventEngine *e,EventChange *c){if(!e||!c||!e->initialized||!e->output_count)return false;*c=e->outputs[e->output_head];e->output_head=(e->output_head+1)%EVENT_OUTPUT_SLOTS;e->output_count--;return true;}
EventResult event_engine_finish(EventEngine *e,EventCloseReason reason,uint64_t now){
 if(!e||!e->initialized||!clock_update(e,now))return EVENT_INVALID;
 if(e->view.exhausted)return EVENT_EXHAUSTED;
 if(active(e)&&!close_event(e,reason))return EVENT_EXHAUSTED;
 reset_background(e);e->have_delivery=false;return EVENT_OK;
}
bool event_engine_confirmed_end(const EventEngine *e,LfnServiceIdentity *id,uint64_t *end){
 if(!e||!id||!end||!e->initialized||!e->have_identity||!e->have_energy)return false;
 *id=e->identity;*end=e->last_end;return true;
}
void event_engine_expire_pending(EventEngine *e){
 if(!e||!e->initialized)return;
 for(unsigned k=0;k<e->pending_count;k++)increment(e,&e->view.expired_pending);
 e->pending_count=0;
}
static bool trigger(EventEngine *e,bool retrigger,uint64_t end){
 EventRecord *r=&e->view.current;
 if(!retrigger){
  if(e->next_id==UINT64_MAX){exhausted(e);return false;}
  memset(r,0,sizeof(*r));r->id=e->next_id++;r->identity=e->identity;r->onset_raw=e->candidate_raw;r->background_power=e->view.background_power;
 }
 if(!increment(e,&r->revision))return false;
 if(!increment(e,&r->trigger_count))return false;
 if(r->trigger_stored<EVENT_TRIGGER_SLOTS)r->triggers[r->trigger_stored++]=e->candidate_raw;else r->triggers_truncated=true;
 r->active_raw=end;r->end_raw=end;e->high_count=e->low_count=0;e->view.state=EVENT_ACTIVE;
 return emit(e,retrigger?EVENT_RETRIGGER:EVENT_OPEN,r);
}
EventResult event_on_energy(EventEngine *e,const LfnServiceMessage *m,uint64_t now){
 if(!e||!e->initialized||!m)return EVENT_INVALID;
 if(e->view.exhausted)return EVENT_EXHAUSTED;
 if(!clock_update(e,now))return EVENT_INVALID;
 if(m->first_delivery>=m->end_delivery||(m->kind!=SERVICE_ENERGY&&m->kind!=SERVICE_DELIVERY_LOSS))return EVENT_INVALID;
 if(e->have_delivery&&m->first_delivery!=e->next_delivery)discontinuity(e,EVENT_SEQUENCE);
 e->have_delivery=true;e->next_delivery=m->end_delivery;
 if(m->kind==SERVICE_DELIVERY_LOSS){discontinuity(e,EVENT_LOSS);return e->view.exhausted?EVENT_EXHAUSTED:EVENT_OK;}
 const PlEnergy *r=&m->energy.record;const LfnServiceIdentity *id=&m->identity;
 if(m->end_delivery-m->first_delivery!=1||!id->session_id||!id->capture_id||!id->config.config_id||id->config.mode!=0||id->config.source>1||id->config.channel>1||id->config.pattern>3||r->capture_id!=id->capture_id||r->config_id!=id->config.config_id){discontinuity(e,EVENT_BAD_INPUT);return EVENT_INVALID;}
 if(e->have_identity&&!same(id,&e->identity))discontinuity(e,EVENT_IDENTITY);
 e->identity=*id;e->have_identity=true;
 if(m->energy.gap){discontinuity(e,EVENT_GAP);return e->view.exhausted?EVENT_EXHAUSTED:EVENT_OK;}
 if(r->seq==UINT64_MAX||r->first_raw>UINT64_MAX-WINDOW){discontinuity(e,EVENT_BAD_INPUT);return exhausted(e);}
 if(r->energy>(UINT64_C(1)<<62)||!(r->flags&LFN_VALID)||(r->flags&~(uint32_t)(LFN_VALID|LFN_SATURATION))){discontinuity(e,EVENT_BAD_INPUT);return EVENT_INVALID;}
 if(e->have_energy&&(r->seq!=e->next_seq||r->first_raw!=e->next_raw))discontinuity(e,EVENT_SEQUENCE);
 if(e->view.exhausted)return EVENT_EXHAUSTED;
 uint64_t end=r->first_raw+WINDOW;
 if(!e->have_energy)e->start_raw=end-HOP;
 e->have_energy=true;e->next_seq=r->seq+1;e->next_raw=r->first_raw+HOP;e->last_end=end;
 double power=(double)r->energy/4611686018427387904.0;
 if(r->flags&LFN_SATURATION){e->high_count=e->low_count=0;if(e->view.state==EVENT_CANDIDATE)e->view.state=EVENT_IDLE;}
 else if(active(e)){
  e->view.current.end_raw=end;
  if(e->view.state==EVENT_ACTIVE){
   if(power<e->view.current.background_power*1.9952623149688795){if(e->low_count<16)e->low_count++;}else e->low_count=0;
   if(e->low_count==16&&end-e->view.current.active_raw>=5*SECOND){e->view.state=EVENT_TAIL;e->tail_raw=end;}
  }else{
   if(power>=e->view.current.background_power*3.9810717055349722){if(!e->high_count)e->candidate_raw=r->first_raw;if(++e->high_count==3)trigger(e,true,end);}else e->high_count=0;

  }
 }else{
  bool ready=background(e,end);
  if(e->view.state==EVENT_WARMUP||!ready){add_background(e,end,power);e->view.state=background(e,end)?EVENT_IDLE:EVENT_WARMUP;e->high_count=0;}
  else if(power>=e->view.background_power*3.9810717055349722){if(!e->high_count)e->candidate_raw=r->first_raw;e->view.state=EVENT_CANDIDATE;if(++e->high_count==3)trigger(e,false,end);}
  else{e->high_count=0;e->view.state=EVENT_IDLE;add_background(e,end,power);background(e,end);}
 }
 /* Deadline progression is independent of valid threshold votes. */
 if(e->view.state==EVENT_TAIL&&end-e->tail_raw>=20*SECOND){close_event(e,EVENT_NORMAL);e->high_count=e->low_count=0;e->view.state=background(e,end)?EVENT_IDLE:EVENT_WARMUP;}
 return e->view.exhausted?EVENT_EXHAUSTED:EVENT_OK;
}
static bool evidence(EventEngine *e,EventRecord *r,const LfnSpectrum *s){
 if((r->identity.config.source!=0)!=s->test_source||r->identity.session_id!=s->session_id||r->identity.capture_id!=s->capture_id||r->identity.config.config_id!=s->config_id||s->end_raw<=r->onset_raw||s->first_raw>=r->end_raw||(r->have_spectrum&&s->stream_seq<=r->last_spectrum_seq))return false;
 if(!increment(e,&r->revision))return false;
 if(!increment(e,&r->spectrum_count))return false;
 r->have_spectrum=true;r->last_spectrum_seq=s->stream_seq;r->last_target_power=s->target_power;r->last_full_power=s->full_range_power;emit(e,EVENT_EVIDENCE,r);return true;
}
EventResult event_on_spectrum(EventEngine *e,const LfnSpectrum *s,uint64_t now){
 if(!e||!s||!e->initialized)return EVENT_INVALID;
 if(e->view.exhausted)return EVENT_EXHAUSTED;
 if(!clock_update(e,now))return EVENT_INVALID;
 if(!s->valid||s->flags!=LFN_VALID||s->first_raw>=s->end_raw||!isfinite(s->target_power)||!isfinite(s->full_range_power)||s->target_power<0||s->full_range_power<s->target_power)return EVENT_INVALID;
 bool found=false;if(active(e))found=evidence(e,&e->view.current,s);
 for(unsigned i=0;i<e->pending_count;i++)found=evidence(e,&e->pending[i].record,s)||found;
 if(!found)increment(e,&e->view.rejected_spectra);
 return e->view.exhausted?EVENT_EXHAUSTED:found?EVENT_OK:EVENT_NO_MATCH;
}






