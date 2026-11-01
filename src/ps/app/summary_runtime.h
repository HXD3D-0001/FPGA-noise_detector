#ifndef LFN_SUMMARY_RUNTIME_H
#define LFN_SUMMARY_RUNTIME_H
#include "storage_multi_sink.h"
#include "marker.h"
enum { SUMMARY_RUNTIME_FILES=4,SUMMARY_RUNTIME_PSD=64,SUMMARY_RUNTIME_RANGES=128,SUMMARY_RUNTIME_OUTPUTS=2 };
typedef struct {uint64_t first,end;} SummaryRange;
typedef struct {SummaryRange ranges[SUMMARY_RUNTIME_RANGES];unsigned count;} SummaryRanges;
typedef struct {uint64_t first,end,seq;double power[10];uint16_t flags;bool used,admitted,synced;} SummaryPsdFact;
typedef struct {bool used;uint64_t file_id,generation;StorageTicket ticket;RecordIdentity identity;} SummaryPending;
typedef struct {MinuteSummary value;StoragePumpTicket pump;StorageTicket receipt;bool used,submitted,admitted,pump_done,synced;} SummaryOutput;
typedef struct {uint64_t id,raw,capture;uint32_t config;bool used;} SummaryMarker;
struct SummaryRuntime;
typedef struct {struct SummaryRuntime *runtime;StorageService *service;uint64_t file_id,generation;unsigned index;} SummaryFileObserver;
typedef struct SummaryRuntime {
 uint64_t session,next_id,generation,cursor,first_raw,fft_first,fft_seq,frontier[3],last_first[3],closed_capture;
 uint64_t source_hold_first,final_end,complete_candidates,summary_count,unknown_loss;
 bool begun,eof,closed,seen[3],source_hold,staged;
 StorageResult error;
 SummaryIdentity identity;StorageMetadata capture_definition;
 StorageTicket definitions[3];uint64_t definition_sequence[3];
 CaptureStorageRoute *route;StorageMultiSink *sink;StoragePump *pumps[SUMMARY_RUNTIME_FILES];
 SummaryFileObserver files[SUMMARY_RUNTIME_FILES];
 SummaryPending pending[SUMMARY_RUNTIME_FILES][STORAGE_RECEIPTS];
 SummaryPsdFact psd[SUMMARY_RUNTIME_PSD],staged_psd;
 SummaryRanges pcm_valid,pcm_saved,available;
 SummaryOutput output[SUMMARY_RUNTIME_OUTPUTS];MinuteSummaryService core;
 MetadataJson scratch;
 SummaryMarker markers[LFN_MARKER_CAPACITY];uint64_t marker_unlinked;
} SummaryRuntime;
/* Single storage owner. Static lifetime, no DMA ownership or allocation.
 * Attach after sink initialization, before cold BEGIN or rotation reconnect.
 * One object per SESSION, never reset at a capture/epoch boundary. */
bool summary_runtime_init(SummaryRuntime *,uint64_t session,uint64_t first_id);
bool summary_runtime_attach(void *,StorageMultiSink *,CaptureStorageRoute *,StoragePump *const *,unsigned,uint64_t generation);
/* Bounded <=16 core slices and two output offers; no backend IO. */
void summary_runtime_step(SummaryRuntime *);
/* Called only after the actual MARKER receipt is SYNCED. References are added
 * to its raw interval's not-yet-submitted summary; late sealed buckets remain
 * immutable and the standalone MARKER remains available to report readers. */
void summary_runtime_marker_synced(SummaryRuntime *,const LfnMarker *);
#endif
