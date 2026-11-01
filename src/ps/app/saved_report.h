#ifndef LFN_SAVED_REPORT_H
#define LFN_SAVED_REPORT_H
#include "noise_report.h"
#include "../storage/storage_metadata.h"
#include "../storage/record_codec.h"

enum { SAVED_REPORT_CONFIGS=8,SAVED_REPORT_CAPTURES=16 };
typedef enum { SAVED_REPORT_OK,SAVED_REPORT_INVALID,SAVED_REPORT_DEPENDENCY,
 SAVED_REPORT_CAPACITY,SAVED_REPORT_UNSUPPORTED } LfnSavedReportError;
typedef struct {
 char hardware[64],gain[64],point_pose[64],adc[64],microphone[64];
 char algorithm[65],rom[65],window[65],bands[64];
 PlCaptureConfig config;uint64_t calibration;bool gain_verified,rate_measured;
 double analog_gain_db,measured_raw_rate;
} LfnSavedReportKey;
typedef struct {StorageMetadata metadata;LfnSavedReportKey key;} LfnSavedReportConfig;
typedef struct {
 uint64_t session,last_sequence,capture,last_end,last_summary_id;
 uint32_t config;unsigned config_count,capture_count,marker_count,summary_count;
 LfnSavedReportError error;char device[64];bool have_session,have_identity,have_range;
 uint64_t duration_ns,valid_ns,expected_candidates,valid_candidates;
 double weighted[SUMMARY_BANDS];
 LfnSavedReportConfig configs[SAVED_REPORT_CONFIGS];
 StorageMetadata captures[SAVED_REPORT_CAPTURES],metadata;
 LfnMarker markers[LFN_MARKER_CAPACITY];LfnMarker marker;
 MetadataJson scratch;MinuteSummary summary;
 LfnLevelSet levels;LfnSavedReportKey key;
} LfnSavedReport;
/* Caller-owned/static accumulator; no heap or backend IO. Feed complete records
 * in file order after record_decode validates framing/CRC. One capture per
 * report, up to8 CONFIG/16 CAPTURE definitions and64 markers. SUMMARY v2 only;
 * revised/overlapping summary intervals are rejected rather than double counted.
 * Local definitions and marker identities are checked before use. */
void lfn_saved_report_init(LfnSavedReport *);
bool lfn_saved_report_add(LfnSavedReport *,const RecordView *);
/* Marker-only files are valid reports; levels are absent until SUMMARY exists.
 * Returned pointers belong to the accumulator and remain valid until reset. */
bool lfn_saved_report_finish(LfnSavedReport *,LfnReportData *);
/* Exact semantic keys are checked before delegating band math. Configuration
 * IDs and declared placeholder config hashes are never treated as content.
 * Values remain full-scale powers, so absolute acoustic levels are unavailable. */
bool lfn_saved_report_compare(LfnSavedReport *,LfnSavedReport *,LfnCompareResult *);
#endif
