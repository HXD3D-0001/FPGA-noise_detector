#ifndef LFN_CAPTURE_CONTROL_H
#define LFN_CAPTURE_CONTROL_H
#include "capture_application.h"
typedef enum { CONTROL_PAUSE=1,CONTROL_RESUME,CONTROL_STOP_CLOSE,CONTROL_ATTACH } CaptureControlKind;
typedef enum { CONTROL_IDLE,CONTROL_QUEUED,CONTROL_EXECUTING,CONTROL_DONE } CaptureControlState;
typedef struct {bool paused;uint64_t pause_token,route_generation;} CaptureControlBarrier;
typedef struct {uint64_t ticket;CaptureControlKind kind;StorageResult result;CaptureControlBarrier barrier;} CaptureControlReceipt;
typedef struct {
 CaptureApplication *application;StorageQueueLocks locks;void *context;
 bool (*is_capture_owner)(void *),(*admit)(void *);uint64_t next_ticket,ticket,pause_token,generation;
 unsigned stop_phase;CaptureControlKind kind;CaptureControlState state;CaptureControlBarrier barrier;
 CaptureControlReceipt receipt,pending;bool pending_result,owner_paused;CaptureStorageRoute *attach_route;
} CaptureControl;
/* Persistent one-slot command/reply mailbox. Nonblocking shared task lock;
 * owner callback authorizes step only, admit under lock excludes legacy work.
 * All callbacks absent locks are for externally serialized Host tests only.
 * PAUSE requires normal STOPPED + producer closed + storage handoff drained.
 * STOP_CLOSE explicitly requests normal STOP once, keeps application.step
 * running until normal STOPPED, then requests input close once. Its successful
 * reply proves capture-side close, NOT storage handoff or durable data.
 * No file IO, notifications, scheduler or implicit START here. */
bool capture_control_init(CaptureControl *,CaptureApplication *,const StorageQueueLocks *,uint64_t seed,bool (*owner)(void *),bool (*admit)(void *),void *);
StorageResult capture_control_submit(CaptureControl *,CaptureControlKind,uint64_t pause_token,uint64_t new_generation,uint64_t *ticket);
/* First cold route, persistent throughout application lifetime. Enqueue is
 * not attachment. Exact success reply proves owner attached + lifetime C7
 * claim; keep reply unACKed while storage stages its gated cold epoch.
 * BUSY includes a foreign C7 lifetime claim, never steals it or auto-times out.
 * No implicit START or hardware-source-off proof. */
StorageResult capture_control_attach(CaptureControl *,CaptureStorageRoute *,uint64_t *ticket);
StorageResult capture_control_copy(CaptureControl *,uint64_t,CaptureControlReceipt *);
StorageResult capture_control_ack(CaptureControl *,uint64_t);
bool capture_control_barrier(CaptureControl *,CaptureControlBarrier *);
/* Capture task calls step, then skips ALL application.step while owner_paused.
 * PAUSE takes effect even if result publication lock is busy. RESUME remains
 * paused until the new barrier/result can be published; never reapply on retry.
 * Caller finishes storage mutations BEFORE submitting RESUME. */
void capture_control_step(CaptureControl *);
bool capture_control_owner_paused(CaptureControl *);
#endif
