#ifndef LFN_RECORD_SCHEMA_H
#define LFN_RECORD_SCHEMA_H
#include <stdint.h>
/* JSON versions are per type; framing/AXI protocol versions are unchanged.
 * CAPTURE v1 is LEGACY_UNCONFIRMED. SUMMARY v1 has FS2*ns weighted sums
 * and fractional counts; only v2 uses nominal paired grid, FS2*s and u64
 * integer candidate counts. Never upgrade a legacy object by relabeling it. */
enum { RECORD_CAPTURE_JSON_V2=2,RECORD_SUMMARY_JSON_V2=2 };
typedef enum {
    RECORD_CONFIG=1, RECORD_CALIBRATION=2, RECORD_TIME_MAP=3, RECORD_PSD=4,
    RECORD_ENERGY=5, RECORD_EVENT_META=6, RECORD_PCM=7, RECORD_MARKER=8,
    RECORD_FAULT=9, RECORD_MINUTE_SUMMARY=10, RECORD_SESSION=11, RECORD_CAPTURE=12
} RecordType;
/* JSON schema codecs will implement the design contract separately. This is
 * the framed binary record boundary, never a serialized native C struct. */
typedef struct {
    RecordType type;
    uint16_t flags;
    uint32_t config_id;
    uint64_t session_id,capture_id,first_raw,end_raw,record_seq;
} RecordIdentity;
#endif
