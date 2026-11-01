#ifndef LFN_STORAGE_CAPTURE_EPOCH_H
#define LFN_STORAGE_CAPTURE_EPOCH_H
#include "storage_multi_sink.h"
#include "storage_segment.h"
#include "../storage/storage_provision.h"
typedef enum { CAPTURE_EPOCH_PREPARE,CAPTURE_EPOCH_ACTIVE,CAPTURE_EPOCH_STOPPING,CAPTURE_EPOCH_HANDOFF,CAPTURE_EPOCH_DONE,CAPTURE_EPOCH_FAILED,CAPTURE_EPOCH_AWAIT_FIRST } StorageCaptureEpochState;
typedef struct {
 StorageFileHandles *handles;StorageFactory *factory;StorageLifecycle *lifecycle;StorageNamespace *space;
 CaptureStorageRoute *route;StorageMultiSink *sink;StorageSegment *segment;unsigned count,slots[STORAGE_PROVISION_FILES];
 void *proof_context;bool (*producer_stopped)(void *);StorageCaptureEpochState state;StorageResult error;bool frozen;
} StorageCaptureEpoch;
/* Pure before provisioning; slots already registered in data/ledger/catalog
 * order. Install returned hooks in FactoryConfig BEFORE provision.init.
 * stopped is a pure owner-synchronized proof (no MMIO/IO): source-off, software
 * drain, no producer calls in progress. Never fake it with a constant on target.
 * All objects persistent/distinct; one storage executor. PREPARE does not step
 * the factory: use provision/lifecycle until definitions SYNCED + enable. */
bool storage_capture_epoch_init(StorageCaptureEpoch *,StorageFileHandles *,StorageFactory *,StorageLifecycle *,StorageNamespace *,const unsigned *,unsigned count,bool (*stopped)(void *),void *,StorageEpochHooks *);
/* Capture owner still stopped. Exact multi-sink and only this factory's pumps;
 * lifetime C7 claim remains with the existing route. No hardware START here. */
StorageResult storage_capture_epoch_attach(StorageCaptureEpoch *,CaptureStorageRoute *,StorageMultiSink *);
/* Storage request only. Capture owner separately requests hardware STOP,
 * flushes losses and calls capture_application_close_storage/producer_close.
 * Until both owner proof AND actual route handoff, rotate cannot start. */
StorageResult storage_capture_epoch_stop(StorageCaptureEpoch *);
/* One route transaction (no backend IO), then <=1 lifecycle backend op.
 * Sole route/factory/lifecycle stepper after attach. Fatal errors retain state,
 * route tickets/pins/fences and any held namespace lease, no auto retry. */
void storage_capture_epoch_step(StorageCaptureEpoch *,uint64_t monotonic_us);
/* After old epoch HANDOFF/all actual closes, pure group unfence, no reopen.
 * One-shot token remains in factory->config.epoch for explicit consume into
 * NEW files; this module never resumes producer or reinitializes live route. */
StorageResult storage_capture_epoch_finish(StorageCaptureEpoch *);
/* Both owners externally paused. old DONE, new PREPARE + enabled lifecycle.
 * New sink object, monotonic public ticket seed; C7 identity unchanged. Does
 * not resume CaptureApplication or hardware. Caller then capture-owner ACKs
 * exact route generation with capture_application_resume_storage. */
StorageResult storage_capture_epoch_reconnect(StorageCaptureEpoch *old_epoch,StorageCaptureEpoch *new_epoch,CaptureStorageRoute *,StorageMultiSink *new_sink);
/* Bind same ordered services, before segment.begin. No backend IO. */
StorageResult storage_capture_epoch_segment(StorageCaptureEpoch *,StorageSegment *);
/* Cold only. Stage while successful ATTACH reply remains unACKed, producer
 * proof still valid, unbound factory and METADATA lifecycle. Does not consume
 * route or step storage. Then ACK allows the explicit first START.
 * Activate only after actual segment definitions SYNCED, first identity bind
 * and lifecycle enable. Running producer is allowed: changes consumer-owned
 * epoch state only, never route/producer initialization. */
StorageResult storage_capture_epoch_stage_first(StorageCaptureEpoch *,CaptureStorageRoute *,StorageMultiSink *);
StorageResult storage_capture_epoch_activate_first(StorageCaptureEpoch *);
#endif
