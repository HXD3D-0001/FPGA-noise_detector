#include "reference_dsp.h"
#include "../../generated/fir_coefficients.h"
#include <string.h>
#include <limits.h>
int64_t lfn_round_q17(int64_t x) {
 uint64_t magnitude=x<0?(uint64_t)(-(x+1))+1:(uint64_t)x;
 uint64_t result=(magnitude+UINT64_C(65536))>>17;
 return x<0?-(int64_t)result:(int64_t)result;
}
int32_t lfn_saturate24(int64_t value,uint8_t *flags,uint64_t *counter) {
 if(value>8388607 || value < -8388608) {
  *flags|=8;
  ++*counter;
  return value>0?8388607:-8388608;
 }
 return (int32_t)value;
}
bool lfn_fir_init(LfnFir *s,const int32_t *c,size_t taps,size_t factor,uint64_t step) {
 if(!s || !c || !taps || taps>67 || !factor || !step) return false;
 memset(s,0,sizeof(*s));s->coefficients=c;s->taps=taps;s->factor=factor;s->anchor_step=step;
 return true;
}
void lfn_fir_clear(LfnFir *s) {
 memset(s->history,0,sizeof(s->history));memset(s->flag_history,0,sizeof(s->flag_history));
 s->phase=0;s->have_previous=false;
 /* Counters survive a stream clear, matching RTL. */
}
bool lfn_fir_push(LfnFir *s,const LfnSample *in,LfnSample *out) {
 if(s->have_previous && (in->capture_id!=s->previous_capture || in->raw_anchor!=s->previous_anchor+s->anchor_step)) lfn_fir_clear(s);
 s->previous_capture=in->capture_id;s->previous_anchor=in->raw_anchor;s->have_previous=true;
 memmove(s->history+1,s->history,(s->taps-1)*sizeof(s->history[0]));
 memmove(s->flag_history+1,s->flag_history,s->taps-1);
 s->history[0]=in->value;s->flag_history[0]=in->flags;
 if(++s->phase!=s->factor) return false;
 s->phase=0;int64_t accumulator=0;uint8_t flags=0;
 for(size_t i=0;i<s->taps;++i) { accumulator+=(int64_t)s->history[i]*s->coefficients[i];flags|=s->flag_history[i]; }
 *out=*in;out->flags=flags;
 out->value=lfn_saturate24(lfn_round_q17(accumulator),&out->flags,&s->saturation_count);
 return true;
}
void lfn_decimator_init(LfnDecimator *s) {
 memset(s,0,sizeof(*s));
 lfn_fir_init(&s->stages[0],lfn_fir_stage1,67,6,1);
 lfn_fir_init(&s->stages[1],lfn_fir_stage2,59,4,6);
 lfn_fir_init(&s->stages[2],lfn_fir_stage3,53,2,24);
}
void lfn_decimator_clear(LfnDecimator *s) {
 for(size_t i=0;i<3;++i) lfn_fir_clear(&s->stages[i]);
 s->have_previous=false;s->output_index=0;
}
bool lfn_decimator_push(LfnDecimator *s,const LfnSample *in,LfnSample *out) {
 LfnFir *first=&s->stages[0];
 if((in->flags&4) || (first->have_previous &&
    (in->capture_id!=first->previous_capture || in->raw_anchor!=first->previous_anchor+1)))
  lfn_decimator_clear(s);
 LfnSample a,b;
 if(!lfn_fir_push(&s->stages[0],in,&a) || !lfn_fir_push(&s->stages[1],&a,&b) || !lfn_fir_push(&s->stages[2],&b,out)) return false;
 if(!s->have_previous || out->capture_id!=s->previous_capture || out->raw_anchor!=s->previous_anchor+48) s->output_index=0;
 if(s->output_index<34) {out->flags|=2;++s->output_index;}
 s->previous_capture=out->capture_id;s->previous_anchor=out->raw_anchor;s->have_previous=true;
 return true;
}
