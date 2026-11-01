#ifndef LFN_STORAGE_RECORDS_H
#define LFN_STORAGE_RECORDS_H
#include "storage_writer.h"
#include "../app/service_bridge.h"
typedef struct {
 const char *device_id;uint64_t session_id,capture_id;PlCaptureConfig config;
 /* Literal immutable ACTIVE anchors/candidate counters, never inferred from
  * first successfully delivered data. Policy must stay immutable to readers. */
 uint64_t first_seq[4],first_raw[4];uint8_t bound_mask;
} StorageRecordPolicy;
/* Supported DATA schemas only: strict canonical v1 ENERGY JSON (u64 strings),
 * 1311 LE float32 PSD, <=4096 LE signed24-in-int32 PCM samples. Metadata has
 * separate schemas; returning false for unsupported types is intentional. */
bool storage_data_validate(void *,const RecordIdentity *,const uint8_t *,size_t);
/* Caller supplies private payload storage; no DMA pointer is retained. Output
 * request and payload remain unchanged on failure. Fill metadata dependency
 * tickets before queue submission. Device IDs are1..63 ASCII identifier bytes. */
bool storage_energy_record(const StorageRecordPolicy *,const LfnServiceMessage *,StorageRequest *,uint8_t *,size_t);
bool storage_psd_record(const StorageRecordPolicy *,const LfnSpectrum *,StorageRequest *,uint8_t *,size_t);
/* View must come from packet_decode; offset/count split RAW into bounded
 * independent blocks. Session is supplied by the explicit matching policy. */
bool storage_pcm_record(const StorageRecordPolicy *,const PacketView *,size_t offset,size_t count,StorageRequest *,uint8_t *,size_t);
typedef struct {
 uint64_t session_id,capture_id,first_raw;uint32_t config_id;uint16_t flags;
 size_t count;const int32_t *samples;
} StoragePcmSamples;
/* Private MONITOR samples, any legal stride48 start; no DMA/stream sequence.
 * <=4096 signed24 values; all are valid. Split history bitmap into valid runs
 * before calling; gaps must never become zero-valued valid samples. */
bool storage_pcm_samples_record(const StorageRecordPolicy *,const StoragePcmSamples *,StorageRequest *,uint8_t *,size_t);
#endif
