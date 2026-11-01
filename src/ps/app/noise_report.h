#ifndef LFN_NOISE_REPORT_H
#define LFN_NOISE_REPORT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "marker.h"
#include "level_compare.h"

/* JSON and text are rendered from the SAME ReportData snapshot, so a report
 * cannot silently disagree between formats. */
typedef enum {LFN_REPORT_SOURCE_UNKNOWN,LFN_REPORT_SOURCE_ADC,LFN_REPORT_SOURCE_TEST} LfnReportSource;
typedef struct {
  uint64_t session_id,capture_id;
  uint32_t config_id;
  bool has_start_utc;
  int64_t start_utc_ns;
  uint64_t start_uncertainty_ns;
  uint64_t duration_ns,valid_ns,expected_ns;
  uint64_t dropped_frames,clipped_samples;
  const LfnMarker *markers;
  unsigned marker_count;
  bool has_comparison;
  const LfnCompareResult *comparison;
  const LfnLevelSet *before,*after;
  /* Optional live snapshot levels; when a persisted comparison is absent this
   * is what the board can still show without inventing a before period. */
  const LfnLevelSet *live;
  bool persisted; /* levels came from immutable SUMMARY records, not RAM */
  LfnReportSource source; /* zero-initialized callers remain explicitly unknown */
  bool adc_left; /* confirmed input used for band-level estimates */
} LfnReportData;

bool lfn_report_json(const LfnReportData *,char *buffer,size_t capacity);
bool lfn_report_text(const LfnReportData *,char *buffer,size_t capacity);
#endif
