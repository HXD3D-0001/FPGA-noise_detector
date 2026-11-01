#ifndef LFN_MINUTE_SUMMARY_H
#define LFN_MINUTE_SUMMARY_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "capture_time_pair.h"
#define LFN_SUMMARY_V2 2
enum { SUMMARY_BANDS=10,SUMMARY_HISTOGRAM=26,SUMMARY_REFERENCES=32 };
typedef struct {uint64_t session_id,capture_id;uint32_t config_id;char device_id[64],band_definition_id[64];bool test_source;} SummaryIdentity;
typedef struct {uint64_t first_ns,end_ns,first_raw,end_raw;bool raw_known,requested,available,valid,saved,pcm_valid;double power[SUMMARY_BANDS],expected_frames,valid_frames;uint16_t flags;
 /* v2: timestamps are derived from raw; counts (0 or 1) belong only to the
  * interval's first_raw, i.e. a tail-hop candidate start. Split spans before
  * offering if they contain multiple candidates or coverage transitions. */
 uint64_t expected_candidates,valid_candidates;bool pcm_saved,availability_known,statistic_known;
} SummaryInterval;
typedef struct {
 SummaryIdentity identity;uint64_t summary_id,revision,first_ns,end_ns,observed_first_ns;
 uint64_t requested_ns,available_ns,valid_ns,unavailable_ns,unsaved_ns,pcm_valid_ticks,first_raw,end_raw;
 bool raw_known,partial,references_truncated;double sum_power_weighted[SUMMARY_BANDS],expected_frames,valid_frames;
 uint64_t histogram_ns[SUMMARY_BANDS][SUMMARY_HISTOGRAM];uint16_t flags;
 uint64_t events[SUMMARY_REFERENCES],markers[SUMMARY_REFERENCES],event_total,marker_total;unsigned event_count,marker_count;
 unsigned schema_version;uint64_t capture_revision,session_start_ns,bucket_start_ns,bucket_end_ns;
 uint64_t quantization_start_ns,quantization_end_ns,expected_candidates,valid_candidates,pcm_saved_ticks;
 uint64_t statistic_unknown_ns,source_available_unknown_ns;
 CaptureTimePair time_pair;
} MinuteSummary;
typedef struct {MinuteSummary current;SummaryInterval input;uint64_t period_ns,cursor,input_cursor,next_id,last_raw_end;bool have_raw,have_input,ready,fault,closed;} MinuteSummaryService;
/* Exact floor(a*b/d), a<=d; safe above2^53 and on32-bitARM. */
bool summary_scale_floor(uint64_t,uint64_t,uint64_t,uint64_t *);
bool minute_summary_init(MinuteSummaryService *,const SummaryIdentity *,uint64_t period_ns,uint64_t first_ns,uint64_t first_summary_id);
/* v2 only: trusted nominal TEST pair, exact revision of an already SYNCED
 * CAPTURE (caller enforces receipt provenance), SESSION-relative buckets.
 * Raw intervals on first_valid_raw + n*48; no unknown raw is invented.
 * first/end_ns are effective grid edges; bucket_* are nominal bucket edges.
 * Quantization offsets describe ceil-grid(bucket edges), even for a clipped
 * partial. Coverage is NOMINAL_GRID, not physical PS fault duration. */
bool minute_summary_init_v2(MinuteSummaryService *,const SummaryIdentity *,const CaptureTimePair *,uint64_t session_start_ns,uint64_t first_valid_raw,uint64_t capture_revision,uint64_t period_ns,uint64_t seed);
/* MONITOR-only, contiguous monotonic intervals; faults explicitly requested
 * but unavailable. Flush on identity change/reboot. Relative FS power only.
 * Frame equivalents may be fractional across buckets; no invented raw clock. */
bool minute_summary_offer(MinuteSummaryService *,const SummaryInterval *);
/* One segment per step, no IO. Power*duration, histogram exact ns; UTC edits
 * never move buckets. Completed output pins progress until take. */
void minute_summary_step(MinuteSummaryService *);
bool minute_summary_take(MinuteSummaryService *,MinuteSummary *);
bool minute_summary_flush(MinuteSummaryService *);
bool minute_summary_reference(MinuteSummaryService *,bool marker,uint64_t id);
#endif
