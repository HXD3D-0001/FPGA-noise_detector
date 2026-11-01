#ifndef LFN_CAPTURE_TIME_PAIR_H
#define LFN_CAPTURE_TIME_PAIR_H
#include "../drivers/capture_regs.h"
#include <stdbool.h>
typedef enum {CAPTURE_TIME_UNCONFIRMED,CAPTURE_TIME_CONFIRMED_NOMINAL,CAPTURE_TIME_REJECTED} CaptureTimeStatus;
typedef struct {
 CaptureTimeStatus status;
 uint64_t session_id,capture_id,anchor_raw,anchor_ns,uncertainty_ns,before_us,after_us;
 uint32_t config_id,source,nominal_rate_hz,a_seq,b_seq;
} CaptureTimePair;
typedef struct {
 CaptureTimePair pair;PlCaptureSnapshot previous;
 uint64_t previous_before_us,last_after_us,last_count,last_raw;uint32_t last_seq;
 bool have_previous,have_last;
} CaptureTimeTracker;
void capture_time_pair_init(CaptureTimeTracker *);
/* Single capture owner; input timestamps directly enclose the CAP1 call.
 * TEST proves generation across A/B. ADC uses the physical-slot counter
 * plus one sample of CDC exclusion at 48 kHz / 3.072 MHz BCLK / 50 MHz proc.
 * The anchor is the digital slot, excluding analog/filter group delay.
 * A valid pair freezes for this identity, including STOP.
 * nominal_rate_hz is not measured; uncertainty bounds only the anchor.
 * Rejected later observations do not erase an already proven historical
 * pair; the return value rejects the observation. Identity changes clear it. */
CaptureTimeStatus capture_time_pair_observe(CaptureTimeTracker *,uint64_t session_id,
 const PlCaptureSnapshot *,uint64_t before_us,uint64_t after_us);
/* Floor nominal mapped nanoseconds, using integer raw differences. Returns
 * false without modifying output for unconfirmed mapping/overflow/underflow. */
bool capture_time_pair_map_ns(const CaptureTimePair *,uint64_t raw,uint64_t *ns);
#endif
