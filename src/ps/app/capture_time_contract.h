#ifndef LFN_CAPTURE_TIME_CONTRACT_H
#define LFN_CAPTURE_TIME_CONTRACT_H
#include "capture_time_pair.h"
/* Persisted v2 contract. This checks consistency of owner-proven facts; it
 * does not manufacture proof or extend anchor uncertainty to clock drift. */
static inline bool capture_time_nominal_valid(const CaptureTimePair *p){
 if(!p||p->status!=CAPTURE_TIME_CONFIRMED_NOMINAL||!p->session_id||!p->capture_id||!p->config_id||p->source>PL_CAPTURE_TEST||p->nominal_rate_hz!=48000||!p->a_seq||p->b_seq<=p->a_seq||p->after_us<p->before_us||p->after_us>=UINT64_MAX/1000)return false;
 uint64_t low=p->before_us*1000,high=(p->after_us+1)*1000,width=high-low;
 return p->anchor_ns==low+width/2&&p->uncertainty_ns==width/2+width%2;
}
static inline const char *capture_time_pair_method(const CaptureTimePair *p){
 return p->source==PL_CAPTURE_ADC?"ADC_COUNTER_CROSSING":"TEST_COUNTER_CROSSING";
}
#endif
