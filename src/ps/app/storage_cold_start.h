#ifndef LFN_STORAGE_COLD_START_H
#define LFN_STORAGE_COLD_START_H
#include "capture_control.h"
#include "storage_capture_epoch.h"
typedef struct {
 void *context;StorageResult (*attach)(void *,CaptureStorageRoute *,uint64_t *);
 StorageResult (*copy)(void *,uint64_t,CaptureControlReceipt *);
 StorageResult (*ack)(void *,uint64_t);
} StorageColdCapturePort;
typedef enum { COLD_SESSION_SUBMIT,COLD_SESSION_WAIT,COLD_ATTACH_SUBMIT,COLD_ATTACH_WAIT,
 COLD_STAGE,COLD_ATTACH_ACK,COLD_WAIT_FACTS,COLD_SEGMENT,COLD_BIND,COLD_ENABLE,
 COLD_ACTIVATE,COLD_SEGMENT_ACK,COLD_DONE,COLD_FAILED } StorageColdState;
typedef struct {
 StorageCaptureEpoch *epoch;CaptureStorageRoute *route;StorageMultiSink *sink;
 StorageColdCapturePort capture;StorageColdState state,failed_at;StorageResult error;
 StorageDefinitionCopy session;StorageMetadataBuilder builder;StorageTicket session_ticket;
 unsigned file;uint64_t ticket,segment_token,last_now;bool have_time;
 CaptureControlReceipt receipt;LfnServiceIdentity identity;
} StorageColdStart;
/* Sole storage owner, persistent private workspace, exclusive capture mailbox
 * client. Provision already METADATA, factory explicitly unbound, empty fresh
 * services, fresh unattached route/sink and ordered IDLE segment on epoch.
 * Caller owns actual initial source-off/software-drain proof (not fabricated
 * here). SESSION facts copied to JSON at init; no external strings retained.
 * init is pure. Rejected init does not transfer component driving ownership.
 * After success ONLY this controller drives lifecycle/segment until DONE.
 * Caller must not submit START until COLD_WAIT_FACTS, nor another START before
 * DONE; capture mailbox transport must exclude legacy requests through ACK.
 * No automatic START, timeout, media creation or failure rollback/reinit. */
bool storage_cold_start_init(StorageColdStart *,StorageCaptureEpoch *,CaptureStorageRoute *,StorageMultiSink *,const StorageColdCapturePort *,const StorageSessionFacts *);
/* WAIT_FACTS only, copies actual route START/ACTIVE snapshot through its lock;
 * explicit config/time facts supplied by platform, no inferred sample anchors.
 * BUSY for unavailable/unstarted facts; token assigned only on admission. */
StorageResult storage_cold_start_segment(StorageColdStart *,const StorageConfigFacts *,const StorageCaptureFacts *,uint64_t *token);
/* <=one abstract backend operation. No route consumption until DONE; then
 * driving transfers to storage_capture_epoch_step. Failures retain tickets,
 * definition prefixes, leases and attachment; no promise of paused capture. */
void storage_cold_start_step(StorageColdStart *,uint64_t monotonic_us);
/* Actual FreeRTOS mailbox adapter, no task creation or IO. */
bool lfn_capture_image_cold_port(StorageColdCapturePort *);
#endif
