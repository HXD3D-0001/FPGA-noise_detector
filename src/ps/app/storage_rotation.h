#ifndef LFN_STORAGE_ROTATION_H
#define LFN_STORAGE_ROTATION_H
#include "capture_control.h"
#include "storage_capture_epoch.h"
typedef struct {
 void *context;
 StorageResult (*submit)(void *,CaptureControlKind,uint64_t,uint64_t,uint64_t *);
 StorageResult (*copy)(void *,uint64_t,CaptureControlReceipt *);
 StorageResult (*ack)(void *,uint64_t);
} StorageRotationCapturePort;
typedef struct {
 StorageProvision *provision;StorageCaptureEpoch *epoch;StorageFactory *factory;
 StorageLifecycle *lifecycle;StorageMultiSink *sink;StorageSegment *segment;
 StorageFactoryConfig config;StorageProvisionOps ops;
 unsigned slots[STORAGE_PROVISION_FILES],targets[4];uint64_t segment_seed;
 StorageMultiAttach attach_observers;void *observer_context;
} StorageRotationNext;
typedef enum {
 ROTATION_IDLE,ROTATION_PAUSE_SUBMIT,ROTATION_PAUSE_WAIT,ROTATION_PAUSE_ACK,
 ROTATION_STOP_OLD,ROTATION_CLOSE_OLD,ROTATION_FINISH_OLD,ROTATION_NEED_NEXT,
 ROTATION_CREATE,ROTATION_BOOTSTRAP_BEGIN,ROTATION_BOOTSTRAP,ROTATION_ENABLE,
 ROTATION_CONNECT,ROTATION_RESUME_SUBMIT,ROTATION_RESUME_WAIT,ROTATION_RESUME_ACK,
 ROTATION_DONE,ROTATION_FAILED,ROTATION_CAPTURE_STOP_SUBMIT,ROTATION_CAPTURE_STOP_WAIT,
 ROTATION_CAPTURE_STOP_ACK,ROTATION_CAPTURE_DRAIN
} StorageRotationState;
typedef struct {
 StorageRotationCapturePort capture;bool (*is_owner)(void *);void *owner_context;
 StorageCaptureEpoch *old;CaptureStorageRoute *route;StorageRotationNext next;
 StorageRotationState state,failed_at;StorageResult error;
 uint64_t ticket,pause_token,generation,last_now;bool paused_proof,have_time;
 CaptureControlReceipt receipt;
} StorageRotation;
/* Sole storage-owner executor, exclusive mailbox client during each rotation.
 * Persistent distinct objects. No START, mount or hardware assumptions.
 * begin takes ACTIVE old epoch stepping. If not already closed/drained, first
 * requests capture-owner normal STOP_CLOSE, keeps old data IO progressing,
 * then drains the route before requesting PAUSE. Failed capture never grants
 * a normal-stop proof. Already drained callers retain the direct PAUSE path.
 * Two disjoint groups must fit the handle cache: 2*(data_count+2)<=HANDLES
 * (currently at most6 data files); otherwise begin returns CAPACITY before
 * issuing PAUSE. Caller also reserves/registers actual free new slots.
 * Caller transfers ALL old epoch stepping to this executor after begin.
 * DONE transfers next epoch stepping back; begin may reuse executor at DONE. */
bool storage_rotation_init(StorageRotation *,const StorageRotationCapturePort *,bool (*owner)(void *),void *);
StorageResult storage_rotation_begin(StorageRotation *,StorageCaptureEpoch *);
/* NEED_NEXT only: explicit NEW registered slots and independent fresh factory
 * workspaces, same count/session, exact old token seed and current identities.
 * Input copied, pointed resources remain exclusive/persistent. Rejected
 * preflight may initialize fresh next epoch/provision objects, never IO or
 * consume token; success consumes old RAM token exactly once. No rollback. */
StorageResult storage_rotation_prepare(StorageRotation *,const StorageRotationNext *);
/* <=one backend op per step. Capture STOP waits keep old route/IO moving;
 * PAUSE/RESUME command/reply waits do not perform backend IO.
 * No shared route/sink mutation after entering RESUME_SUBMIT. Failed reply
 * remains unACKed for diagnosis, failures retain files/lease/pins/capability.
 * BUSY waits have no implicit timeout; external owner can inspect/quarantine.
 * Monotonic regression fails closed. No concurrent external stepping. */
void storage_rotation_step(StorageRotation *,uint64_t monotonic_us);
/* Latched successful PAUSE proof, not a try-lock read of the public barrier.
 * Bound to old/new epoch hooks by executor; false before resume submission. */
bool storage_rotation_stopped(void *);
/* Actual task mailbox adapter; does not start a task or perform IO. */
bool lfn_capture_image_rotation_port(StorageRotationCapturePort *);
#endif
