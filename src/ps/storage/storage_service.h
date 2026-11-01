#ifndef LFN_STORAGE_SERVICE_H
#define LFN_STORAGE_SERVICE_H
#include "storage_metadata.h"
#include "event_meta.h"
#include "marker_record.h"
#include "storage_queue.h"
/* All APIs belong to ONE task, including submit/take/poll/recovery. No direct
 * queue producers or registry writers after attach. No backend IO in offer. */
typedef struct {
 bool occupied,definition,observed,audit;StorageReceiptState state;
 StorageTicket ticket;RecordIdentity identity;uint64_t object_id,calibration_id;
 bool observer_failed,terminal_notified;
} StorageServiceSlot;
/* Sole-owner callbacks: fixed state only, no IO, recursive step or submit.
 * Admission follows queue copy and stable slot publication. Terminal precedes
 * auto-ACK; false retains the exact receipt and latches fatal. */
typedef struct {
 void *context;
 bool (*admission)(void *,const StorageServiceSlot *,const StorageRequest *);
 bool (*terminal)(void *,const StorageServiceSlot *);
} StorageServiceObserver;
typedef struct {
 uint64_t accepted,synced,failed,capacity_rejected,invalid,delivery_notices,source_notices;
 bool sealed,drained,fatal,pending;StorageResult pending_result;
} StorageServiceHealth;
enum { STORAGE_SERVICE_PAYLOAD_BYTES=4096*4,STORAGE_INGRESS_FULL=1,STORAGE_INGRESS_LOCK=2,STORAGE_INGRESS_OVERFLOW=4 };
typedef struct {LfnServiceIdentity identity;uint32_t stream,reason;uint64_t first_raw,end_raw,stream_seq,count;} StorageIngressLoss;
typedef struct {uint64_t session_id,capture_id,first_raw,end_raw;uint32_t config_id;size_t first_sample,sample_count;} StoragePcmRemainder;
typedef struct {
 StorageQueue *queue;StorageMetadataRegistry *registry;
 StorageServiceSlot slots[STORAGE_RECEIPTS];
 StorageUnsavedLedger unsaved,inflight;StorageTicket audit_ticket;
 uint64_t next_fault_id;bool audit_active;
 MetadataJson scratch;uint8_t payload[STORAGE_SERVICE_PAYLOAD_BYTES];LfnServiceMessage pending;
 /* Complete private packet copy; no borrowed view survives stage_pcm. */
 uint8_t pcm_packet[LFN_MAX_PACKET];size_t pcm_bytes,pcm_next;uint64_t pcm_session;
 bool pcm_pending;
 StorageServiceHealth health;
 StorageServiceObserver observer;
} StorageService;
/* Fresh service, empty queue/registry, READY writer, production schema gate.
 * Initialize ONCE; do not reset ticket generations/identity mid-session. */
bool storage_service_init(StorageService *,StorageQueue *,StorageMetadataRegistry *);
/* Sole service owner; every retained definition observed SYNCED, matching
 * local SESSION/CONFIG/CAPTURE and OPEN policy. Pure, no IO or state change. */
bool storage_service_capture_ready(StorageService *,uint64_t session,uint32_t config,uint64_t capture);
StorageSubmit storage_service_metadata(StorageService *,const RecordIdentity *,const uint8_t *,size_t);
/* Reserve-admission variant used only for the bounded EVENT-roll bootstrap
 * closure after the old file has been released; normal producers never use it. */
StorageSubmit storage_service_metadata_reserved(StorageService *,const RecordIdentity *,const uint8_t *,size_t,bool);
/* ENERGY, source GAP and delivery LOSS by value. BUSY/CAPACITY/DEPENDENCY
 * are not consumed; caller keeps its value and retries or explicitly discards.
 * Any assigned rejected sequence is still recorded as UNSAVED. */
/* Caller supplies private stable spectrum (e.g. snapshot_copy). Return OK
 * means copied queue acceptance only; no pointer is retained on any return. */
StorageSubmit storage_service_ingress_loss(StorageService *,const StorageIngressLoss *);
/* Queue OK copies immediately. Caller advances append_cursor only on OK;
 * SYNCED/FAILED comes exclusively from the exact admission/terminal observer. */
StorageSubmit storage_service_pcm_samples(StorageService *,const StoragePcmSamples *);
StorageSubmit storage_service_event_meta(StorageService *,const StorageEventMeta *);
StorageSubmit storage_service_marker(StorageService *,const LfnMarker *);
StorageSubmit storage_service_summary(StorageService *,const MinuteSummary *);
StorageSubmit storage_service_psd(StorageService *,const LfnSpectrum *);
/* Stage copies/validates a complete PCM/RAW packet. OK means RAM-staged,
 * not queued/saved. Retry stage only if it was not accepted. */
StorageResult storage_service_stage_pcm(StorageService *,uint64_t session_id,const uint8_t *,size_t);
/* One <=4096-sample piece per call. Advances only on queue OK; BUSY/rejection
 * keeps exact next offset. EXPIRED means no pending packet. */
StorageSubmit storage_service_pcm_next(StorageService *);
/* Explicitly return unsubmitted remainder facts; prior accepted pieces retain
 * their independent receipt lifecycle. Does NOT claim a persisted GAP. */
bool storage_service_discard_pcm(StorageService *,StoragePcmRemainder *);
StorageSubmit storage_service_offer(StorageService *,const LfnServiceMessage *);
/* One logical C7 consumer, at most one take. Retains even invalid input for
 * explicit inspection/discard; retries retained input before taking another. */
StorageResult storage_service_take(StorageService *,LfnServiceBridge *);
bool storage_service_discard_pending(StorageService *,LfnServiceMessage *);
/* Caller first stops/flushes producer and drains C7 through take until EMPTY.
 * Seal refuses retained input; freeze new input, not hardware capture STOP. */
bool storage_service_seal(StorageService *);
/* At most one queue/E1 IO operation. Auto-ACK data; retain definitions until
 * explicit release. UNSAVED snapshot cleared ONLY after its exact SYNCED. */
void storage_service_step(StorageService *,uint64_t now_us);
StorageResult storage_service_release(StorageService *,StorageTicket);
/* Explicit ownership re-established by caller; never truncates automatically. */
bool storage_service_recover(StorageService *);
bool storage_service_repair_tail(StorageService *);
#endif
