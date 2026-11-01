#include "capture_time_pair.h"
#include <string.h>
void capture_time_pair_init(CaptureTimeTracker *t){memset(t,0,sizeof(*t));}
static CaptureTimeStatus reject(CaptureTimeTracker *t){
 t->have_previous=false;
 if(t->pair.status!=CAPTURE_TIME_CONFIRMED_NOMINAL)t->pair.status=CAPTURE_TIME_REJECTED;
 return CAPTURE_TIME_REJECTED;
}
CaptureTimeStatus capture_time_pair_observe(CaptureTimeTracker *t,uint64_t session,const PlCaptureSnapshot *s,uint64_t before,uint64_t after){
 if(!t||!s)return CAPTURE_TIME_REJECTED;
 CaptureTimePair *p=&t->pair;
 if(p->session_id!=session||p->capture_id!=s->capture_id||p->config_id!=s->active_config_id||p->source!=s->configured.source){
  capture_time_pair_init(t);p->session_id=session;p->capture_id=s->capture_id;
  p->config_id=s->active_config_id;p->source=s->configured.source;p->nominal_rate_hz=48000;
 }
 /* RTL CAP1 flags: mode[29:28], historical/current fault[27:24]/[23:20],
  * frontend[19:16], operation[11:9], and individual status bits[8:0]. */
 const uint32_t known=UINT32_C(0x3fff0fff);
 if(!session||!s->capture_id||!s->active_config_id||!s->seq||s->configured.config_id!=s->active_config_id||
    s->configured.source>PL_CAPTURE_TEST||s->configured.mode>2||s->configured.channel>1||s->configured.pattern>3||
    (s->flags&~known)||(s->flags&PL_CAPTURE_F_QUARANTINED)||((s->flags>>16)&15)>=7||
    ((s->flags>>20)&15)||((s->flags>>9)&7)>PL_CAPTURE_OP_RECOVER||
    ((s->flags>>28)&3)>2||
    ((s->flags&PL_CAPTURE_F_RUNNING)&&((s->flags&PL_CAPTURE_F_STOP_COMPLETE)||
      ((s->flags>>16)&15)!=5||((s->flags>>28)&3)!=s->configured.mode))||
    ((s->flags&PL_CAPTURE_F_TEST)!=0)!=(s->configured.source==PL_CAPTURE_TEST)||
    after<before||after>=UINT64_MAX/1000||
    (t->have_last&&(before<t->last_after_us||s->seq<=t->last_seq||
      s->configured.mode!=t->previous.configured.mode||s->configured.channel!=t->previous.configured.channel||
      s->configured.pattern!=t->previous.configured.pattern)))return reject(t);
 uint64_t count=s->configured.source==PL_CAPTURE_TEST?s->test_count:s->adc_count;
 uint64_t guard=s->configured.source==PL_CAPTURE_ADC?1:0;
 if(count>UINT64_MAX-s->origin_raw||guard>UINT64_MAX-s->origin_raw-count||
    ((s->flags&PL_CAPTURE_F_POSITION_VALID)&&(!count||s->latest_raw<s->origin_raw||s->latest_raw>=s->origin_raw+count+guard))||
    (t->have_last&&(s->origin_raw!=t->previous.origin_raw||count<t->last_count||s->latest_raw<t->last_raw)))return reject(t);
 t->last_after_us=after;t->last_seq=s->seq;t->last_count=count;t->last_raw=s->latest_raw;t->have_last=true;
 if(p->status==CAPTURE_TIME_CONFIRMED_NOMINAL)return p->status;
 p->status=CAPTURE_TIME_UNCONFIRMED;
 if((s->flags&(PL_CAPTURE_F_RUNNING|PL_CAPTURE_F_POSITION_VALID))!=(PL_CAPTURE_F_RUNNING|PL_CAPTURE_F_POSITION_VALID)||
    (s->flags&(PL_CAPTURE_F_SERVICE_BUSY|PL_CAPTURE_F_CLOCK_BUSY))){
  t->previous=*s;t->have_previous=false;return p->status;
 }
 if(!t->have_previous){t->previous=*s;t->previous_before_us=before;t->have_previous=true;return p->status;}
 /* ADC physical_count crosses a registered BCLK Gray stage and two 50 MHz
  * proc stages. At the supported 3.072 MHz BCLK / 48 kHz frame rate this
  * trails by less than one selected-channel sample. Exclude that tail as
  * well; downstream FIFO latency may delay B but cannot move generation A.
  * This pairs the digital slot index, not ADC analog/filter group delay. */
 uint64_t previous_count=s->configured.source==PL_CAPTURE_TEST?t->previous.test_count:t->previous.adc_count;
 uint64_t threshold=t->previous.origin_raw+previous_count+guard;
 if(s->latest_raw<threshold)return p->status;
 /* The A counter excludes the threshold sample; B has accepted that sample
  * or a later one. Integer microsecond quantization expands the upper edge. */
 uint64_t low=t->previous_before_us*1000,high=(after+1)*1000,width=high-low;
 p->anchor_raw=s->latest_raw;p->anchor_ns=low+width/2;p->uncertainty_ns=width/2+width%2;
 p->before_us=t->previous_before_us;p->after_us=after;p->a_seq=t->previous.seq;p->b_seq=s->seq;
 p->status=CAPTURE_TIME_CONFIRMED_NOMINAL;return p->status;
}
bool capture_time_pair_map_ns(const CaptureTimePair *p,uint64_t raw,uint64_t *ns){
 if(!p||!ns||p->status!=CAPTURE_TIME_CONFIRMED_NOMINAL||p->nominal_rate_hz!=48000)return false;
 bool negative=raw<p->anchor_raw;uint64_t d=negative?p->anchor_raw-raw:raw-p->anchor_raw;
 uint64_t q=d/48000,r=(d%48000)*UINT64_C(1000000000),fraction=r/48000;
 if(negative&&r%48000)fraction++;
 if(q>UINT64_MAX/UINT64_C(1000000000))return false;
 uint64_t whole=q*UINT64_C(1000000000);
 if(fraction>UINT64_MAX-whole)return false;
 d=whole+fraction;
 if(negative){if(d>p->anchor_ns)return false;*ns=p->anchor_ns-d;}
 else{if(d>UINT64_MAX-p->anchor_ns)return false;*ns=p->anchor_ns+d;}
 return true;
}
