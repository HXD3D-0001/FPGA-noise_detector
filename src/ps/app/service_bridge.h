#ifndef LFN_SERVICE_BRIDGE_H
#define LFN_SERVICE_BRIDGE_H
#include "analysis_service.h"
#include "raw_capture.h"
#include "capture_runtime.h"
enum { LFN_SERVICE_ENERGY_SLOTS=256, LFN_DELIVERY_QUEUE_FULL=1, LFN_DELIVERY_LOCK_BUSY=2 };
typedef enum { SERVICE_OK,SERVICE_EMPTY,SERVICE_BUSY,SERVICE_INVALID,SERVICE_EXHAUSTED } LfnServiceResult;
typedef struct {
 void *context;
 bool (*queue_lock)(void *);void (*queue_unlock)(void *);
 bool (*snapshot_lock)(void *);void (*snapshot_unlock)(void *);
} LfnServiceLocks;
typedef struct {uint64_t session_id,capture_id;PlCaptureConfig config;} LfnServiceIdentity;
typedef enum { SERVICE_ENERGY=1,SERVICE_DELIVERY_LOSS=2 } LfnServiceMessageKind;
typedef struct {
 LfnServiceMessageKind kind;
 /* Delivery ordinals, not sample indices. LOSS has no invented capture/raw
  * range and may span identities. End is exclusive. */
 uint64_t first_delivery,end_delivery;
 LfnServiceIdentity identity;CaptureEnergyMessage energy;uint32_t loss_reason;
} LfnServiceMessage;
typedef struct {
 uint64_t attempted,queued,dropped,loss_notices,invalid,snapshot_skipped,unsequenced;
 unsigned queue_highwater;bool delivery_exhausted,snapshot_exhausted,pending_loss;
} LfnServiceHealth;
typedef struct {
 uint64_t version,published_us,requested_session;
 PlCaptureConfig requested;
 bool prepared_valid,capture_valid;LfnServiceIdentity prepared,active;
 uint32_t application_state,runtime_state;int32_t failure;
 uint64_t packets[3],energy_records,energy_gaps;
 LfnRawStatus raw;
 LfnAnalysisSnapshot analysis;LfnServiceHealth transport;
} LfnServiceSnapshot;
typedef struct {
 void *context;
 void (*energy)(void *,const LfnServiceIdentity *,const CaptureEnergyMessage *);
 void (*snapshot)(void *,const LfnServiceSnapshot *);
} LfnServiceProducerObserver;
typedef struct LfnServiceBridge {
 LfnServiceLocks locks;bool initialized,have_snapshot;
 LfnServiceProducerObserver observer;
 /* Private to sole producer, even when locks are present. Consumer functions
  * touch only the guarded published snapshot and queue. No native struct is
  * an on-disk or network wire representation. */
 uint64_t next_delivery,next_snapshot,loss_first,loss_end;uint32_t loss_reason;
 bool pending_loss;LfnServiceHealth health;
 LfnServiceSnapshot staging,published;
 LfnServiceMessage queue[LFN_SERVICE_ENERGY_SLOTS];unsigned head,count;
 const void *consumer_owner; /* guarded by queue lock; lifetime claim */
} LfnServiceBridge;
/* All callbacks absent means externally serialized use. Otherwise all four
 * are required, bounded try-locks shared by producer and consumer tasks. */
bool lfn_service_bridge_init(LfnServiceBridge *,const LfnServiceLocks *);
/* Set only before sharing or with the producer paused; NULL removes hooks.
 * Hooks run synchronously on the sole producer before queue/snapshot locks,
 * including when delivery is full/busy. Borrowed inputs expire on return.
 * Hooks must be bounded, nonblocking and must not reenter this bridge. */
bool lfn_service_producer_observer(LfnServiceBridge *,const LfnServiceProducerObserver *);
/* Sole producer: data copied by value, never retains source/DMA pointers. */
LfnServiceResult lfn_service_energy_offer(LfnServiceBridge *,const LfnServiceIdentity *,const CaptureEnergyMessage *);
/* Pump terminal LOSS even without later data or while application is stopped.
 * Queue contents do not own DMA and never block capture STOP/new sessions. */
LfnServiceResult lfn_service_flush(LfnServiceBridge *);
/* Sole producer fills staging; publish copies under a try-lock. */
LfnServiceResult lfn_service_snapshot_publish(LfnServiceBridge *,uint64_t now_us);
/* Task consumers: output is caller-owned, untouched on EMPTY/BUSY/error.
 * Queue is for one logical consumer; snapshots can have multiple readers. */
LfnServiceResult lfn_service_snapshot_copy(LfnServiceBridge *,LfnServiceSnapshot *);
LfnServiceResult lfn_service_consumer_claim(LfnServiceBridge *,const void *owner);
/* Both owners stopped: release the exact claim only when the queue and pending
 * loss are empty. Preserves delivery/snapshot counters across sessions. */
LfnServiceResult lfn_service_consumer_release(LfnServiceBridge *,const void *owner);
LfnServiceResult lfn_service_energy_take_owned(LfnServiceBridge *,const void *owner,LfnServiceMessage *);
LfnServiceResult lfn_service_energy_take(LfnServiceBridge *,LfnServiceMessage *);
/* Non-consuming queue check. Producer externally paused; pending_loss is
 * producer-owned and must also be resolved before this cross-owner barrier. */
LfnServiceResult lfn_service_consumer_empty(LfnServiceBridge *,const void *);
#endif
