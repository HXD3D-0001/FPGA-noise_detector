#ifndef LFN_STORAGE_LIFECYCLE_H
#define LFN_STORAGE_LIFECYCLE_H
#include "storage_factory.h"
#include "storage_namespace.h"
typedef enum { LIFECYCLE_ASSEMBLE,LIFECYCLE_METADATA,LIFECYCLE_RUNNING,LIFECYCLE_WAIT_STOP,LIFECYCLE_ROTATE,LIFECYCLE_HANDOFF,LIFECYCLE_FAILED } StorageLifecycleState;
typedef struct {
 StorageFactory *factory;StorageNamespace *space;StorageLifecycleState state;
 uint64_t lease,last_now;unsigned round_robin;bool have_time;
} StorageLifecycle;
/* Already init'ed fresh factory; no file IO. Must be sole factory/engine
 * stepper afterward. Snapshot may coexist with normal data writes, never
 * OPEN/SEAL/RET mutation. Factory/bootstrap/rotation hold full write lease.
 * Caller continues inventory.step to release a read lease, or stop waits.
 * Hardware STOP/route.producer_close remain capture-owner actions. */
bool storage_lifecycle_init(StorageLifecycle *,StorageFactory *,StorageNamespace *);
/* One-way transfer from the sole provisioner, before first assembly step. */
StorageResult storage_lifecycle_adopt_write(StorageLifecycle *,uint64_t token);
void storage_lifecycle_step(StorageLifecycle *,uint64_t monotonic_us);
/* COLD metadata and rotation bootstrap both require own local definitions'
 * exact public SYNCED, open matching capture/config policy. Permission only,
 * never hardware START. Failure returns BUSY and leaves phase unchanged. */
StorageResult storage_lifecycle_enable(StorageLifecycle *);
StorageResult storage_lifecycle_bootstrap(StorageLifecycle *,const StorageBootstrapPlan *const *);
/* Freeze start permission, caller performs STOP/flush/route handoff. Lifecycle
 * waits hooks.quiescent, then closes/drains engine under namespace write lease.
 * HANDOFF is old epoch closed; new factory uses one-shot token/new IDs.
 * Failures retain any held write lease for explicit owner recovery. */
StorageResult storage_lifecycle_request_stop(StorageLifecycle *);
#endif
