#ifndef LFN_TIME_MAP_H
#define LFN_TIME_MAP_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
enum { LFN_TIME_MAP_CAPACITY=8, LFN_GROUP_DELAY_RAW_TICKS=831 };
typedef struct {
    uint64_t session_id,capture_id,raw_index;
    uint32_t config_id;
    bool group_delay_compensated;
} LfnSampleStamp;
typedef struct {
    uint64_t map_id,session_id,capture_id,valid_from_raw;
    uint32_t config_id;
    int64_t utc_at_valid_from_ns;
    double raw_fs_hz,fs_uncertainty_ppm;
    uint64_t base_uncertainty_ns;
    /* Metadata only; apply neither implicitly nor twice. */
    int64_t adc_delay_ns,analog_delay_ns;
} LfnTimeMapEntry;
typedef struct { size_t count; LfnTimeMapEntry entries[LFN_TIME_MAP_CAPACITY]; } LfnTimeMap;
typedef struct { int64_t utc_ns; uint64_t map_id,uncertainty_ns; } LfnUtcEstimate;
typedef struct {
    bool have_previous,monitor_requested,available;
    uint64_t last_monotonic_ns,monitor_requested_ns,available_ns;
} LfnMonotonicCoverage;
typedef struct {
    bool have_previous;
    uint64_t session_id,capture_id,end_raw,valid_raw_ticks;
    uint32_t config_id;
} LfnRangeCoverage;
bool lfn_time_map_append(LfnTimeMap *,const LfnTimeMapEntry *);
bool lfn_time_to_utc(const LfnTimeMap *,LfnSampleStamp,LfnUtcEstimate *);
bool lfn_stamp_compensate_group_delay(LfnSampleStamp,LfnSampleStamp *);
/* Caller keeps relative sample/monotonic axes when no UTC map applies. */
bool lfn_u64_decimal(uint64_t,char *destination,size_t capacity);
/* Each call closes the PREVIOUS state interval before recording new state.
 * UTC edits cannot change this denominator. Fault time remains requested. */
bool lfn_monotonic_coverage_tick(LfnMonotonicCoverage *,uint64_t now_ns,
                                  bool monitor_requested,bool available);
/* Use independent instances for spectrum, PCM and saved coverage. Caller
 * submits only validated ranges and uses their config Fs to derive seconds.
 * Same-capture windows must arrive in nondecreasing end order. */
bool lfn_range_coverage_add(LfnRangeCoverage *,LfnSampleStamp first,uint64_t end_raw);
#endif
