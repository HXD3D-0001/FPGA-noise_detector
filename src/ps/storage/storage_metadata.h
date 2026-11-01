#ifndef LFN_STORAGE_METADATA_H
#define LFN_STORAGE_METADATA_H
#include "storage_records.h"
#include "../app/minute_summary.h"
#include "metadata_json.h"
#include <stdatomic.h>
typedef enum {STORAGE_TIME_LEGACY_UNCONFIRMED,STORAGE_TIME_NOMINAL_PAIRED} StorageTimeQuality;
typedef struct {
 RecordIdentity identity;char device_id[64],hardware_id[64],gain_setting_id[64];
 PlCaptureConfig config;uint64_t calibration_id,object_id,revision,origin_raw,first_valid_raw;
 uint64_t first_seq[4],first_raw[4],start_ns,end_ns,mono_raw,mono_ns,uncertainty;
 uint8_t mask;bool closed,calibrated,synchronized,gain_verified;double slope;
 unsigned schema_version;StorageTimeQuality time_quality;CaptureTimePair time_pair;
 bool summary_histogram; /* explicit v2 full-scale-sine histogram contract */
} StorageMetadata;
/* Caller-owned scratch: metadata decode can use~8KiB, not capture-task stack. */
bool storage_metadata_decode(const RecordIdentity *,const uint8_t *,size_t,MetadataJson *,StorageMetadata *);
/* Validation + copied payload, no allocation/IO, output unchanged on failure. */
bool storage_metadata_record(const RecordIdentity *,const uint8_t *,size_t,MetadataJson *,StorageRequest *,uint8_t *,size_t);
typedef struct {StorageMetadata value;size_t bytes;uint8_t payload[STORAGE_METADATA_BYTES];} StorageMetadataEntry;
typedef struct {
 /* Sole writer fully initializes a new entry, then release-publishes count.
  * Readers acquire one fixed count and never observe overwritten entries. */
 atomic_uint published;
 StorageMetadataEntry entries[STORAGE_METADATA_ENTRIES];
} StorageMetadataRegistry;
typedef struct {char device_id[64];StorageRecordPolicy policy;bool closed;uint64_t end_raw;} StoragePolicyCopy;
void storage_registry_init(StorageMetadataRegistry *);
/* Sole owner: apply only actual accepted metadata or complete replayed records.
 * record_seq must be nonzero. Caller establishes queue/recovery provenance.
 * No eviction: capacity is an explicit failure, never overwrites history. */
/* Sole writer preflight, without publication. Use next actual queue sequence;
 * no concurrent registry writer/admission between preflight and apply. */
StorageMetaResult storage_registry_preflight(const StorageMetadataRegistry *,const RecordIdentity *,const uint8_t *,size_t,MetadataJson *);
StorageMetaResult storage_registry_apply(StorageMetadataRegistry *,const RecordIdentity *,const uint8_t *,size_t,MetadataJson *);
StorageMetaResult storage_registry_policy(const StorageMetadataRegistry *,uint64_t,uint64_t,uint32_t,uint64_t before_sequence,StoragePolicyCopy *);
/* Exact historical revision; excludes definitions at/after before_sequence.
 * This is a semantic query, NOT proof of SYNCED; service receipt owns that. */
StorageMetaResult storage_registry_capture(const StorageMetadataRegistry *,uint64_t session,uint64_t capture,uint32_t config,uint64_t revision,uint64_t before_sequence,StorageMetadata *);
/* Pure, multi-reader backend gate. DATA uses immutable historical policy;
 * metadata parses in local scratch. No state is applied by validation. */
/* Type10 JSON, exact u64 strings and finite linear weighted powers. Decoder
 * checks internal coverage/histogram consistency; registry adds local policy. */
bool storage_summary_decode(const RecordIdentity *,const uint8_t *,size_t,MetadataJson *,MinuteSummary *);
bool storage_summary_record(const MinuteSummary *,StorageRequest *,uint8_t *,size_t);
bool storage_registry_validate(void *,const RecordIdentity *,const uint8_t *,size_t);
typedef struct {
 uint64_t session_id,capture_id,first_raw,end_raw,first_sequence,end_sequence;
 uint32_t config_id;bool sample_range_known;
} StorageUnsavedRange;
typedef struct {StorageUnsavedRange ranges[STORAGE_UNSAVED_RANGES];unsigned count;uint64_t overflow;} StorageUnsavedLedger;
/* Serialized service ledger. Never merges over successful sequence holes. */
bool storage_unsaved_add(StorageUnsavedLedger *,const StorageUnsavedRange *);
bool storage_unsaved_record(const char *device_id,uint64_t session_id,uint64_t fault_id,
 uint64_t mono_start_ns,uint64_t mono_end_ns,const StorageUnsavedLedger *,MetadataJson *,StorageRequest *,uint8_t *,size_t);
#endif
