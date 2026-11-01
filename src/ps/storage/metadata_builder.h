#ifndef LFN_METADATA_BUILDER_H
#define LFN_METADATA_BUILDER_H
#include "storage_metadata.h"
/* Private caller-owned work area, not a capture stack or DMA buffer.
 * Builders require independent output buffers and leave output unchanged on
 * failure. Strings are bounded ASCII identifiers/hashes, not escaped JSON. */
typedef struct {MetadataJson scratch;uint8_t bytes[STORAGE_METADATA_BYTES];size_t used;bool ok;} StorageMetadataBuilder;
typedef struct {const char*device_id;uint64_t session_id,boot_generation,start_ns;const char*build_hash;bool temporary;} StorageSessionFacts;
typedef struct {double high_db,low_db,background_seconds,min_active_seconds,tail_seconds,power_floor;uint64_t high_updates,low_updates;} StorageEventFacts;
typedef struct {
 const char *device_id,*config_hash,*hardware_id,*gain_setting_id,*adc_id,*microphone_id,*point_pose_id,*band_definition_id;
 uint64_t session_id,calibration_id;PlCaptureConfig config;
 bool gain_verified,fs_measured;double gain_db,measured_raw_fs_hz,fs_uncertainty_ppm;
 StorageEventFacts event;
} StorageConfigFacts;
typedef struct {
 const char*device_id;PlCaptureConfig config;
 /* Copies made by the actual sole capture owner. Never read concurrently from
  * live runtime/CaptureTask and never infer anchors from first data received. */
 PlCaptureSnapshot started;PlContractSnapshot active;uint64_t max_age_ticks;
 uint64_t first_valid_raw,start_ns,mono_raw,mono_ns,uncertainty_ns;
 double ns_per_raw;uint64_t revision;
 bool closed,software_drained;PlCaptureSnapshot stopped;
 uint64_t end_raw,end_ns;const char*end_reason;
 /* 0/1 explicitly retains the legacy builder; production requests v2 and
  * passes the sole owner's by-value time_pair. No fallback to observed_us. */
 unsigned schema_version;CaptureTimePair time_pair;
} StorageCaptureFacts;
typedef struct {
 const char*device_id,*clock_source;uint64_t session_id,capture_id;uint32_t config_id;
 uint64_t map_id,anchor_raw,uncertainty_ns;double ns_per_raw;bool synchronized;int64_t utc_ns;
} StorageTimeFacts;
bool storage_build_session(StorageMetadataBuilder *,const StorageSessionFacts *,StorageRequest *,uint8_t *,size_t);
bool storage_build_config(StorageMetadataBuilder *,const StorageConfigFacts *,StorageRequest *,uint8_t *,size_t);
bool storage_build_capture(StorageMetadataBuilder *,const StorageCaptureFacts *,StorageRequest *,uint8_t *,size_t);
bool storage_build_time(StorageMetadataBuilder *,const StorageTimeFacts *,StorageRequest *,uint8_t *,size_t);
#endif
