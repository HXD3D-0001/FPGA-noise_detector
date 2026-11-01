#ifndef LFN_TIMER_SCALE_H
#define LFN_TIMER_SCALE_H
#include <stdint.h>
/* Pass the positive BSP frequency as a value: its macro may be an ungrouped
 * expression. Split seconds/remainder to avoid overflowing ticks * 1000000. */
static inline uint64_t lfn_timer_ticks_us(uint64_t ticks,uint64_t frequency){
 return (ticks/frequency)*UINT64_C(1000000)+(ticks%frequency)*UINT64_C(1000000)/frequency;
}
#endif
