#ifndef LFN_STORAGE_ENGINE_H
#define LFN_STORAGE_ENGINE_H
#include "storage_pump.h"
#include "storage_epoch.h"
#include "storage_query.h"
#include "storage_delete.h"
typedef enum { ENGINE_ACTIVE,ENGINE_DRAINING,ENGINE_ROTATING,ENGINE_HANDOFF,ENGINE_FAILED,ENGINE_AWAIT_CAPTURE } StorageEngineState;
typedef struct {
 StorageFileCatalog *catalog;StoragePump *pumps[STORAGE_FAMILY_FILES];unsigned count,round_robin,budget;
 StorageQuery *query;StorageDelete *deletion;StorageEpoch *epoch;
 StorageBootstrapPlan *plans[STORAGE_FAMILY_FILES];uint32_t config[STORAGE_FAMILY_FILES];uint64_t capture[STORAGE_FAMILY_FILES];
 StorageEpochHooks hooks;StorageEngineState state;uint64_t steps;bool rotation_due;
 uint64_t max_file_bytes,max_file_age_us,created_us;
} StorageEngine;
/* Platform-neutral sole storage task body. All objects persistent and supplied;
 * no malloc/RTOS/SD/controller/drive assumptions. Matching pump/service order,
 * same shared family, query/deletion backend hooks MUST share this owner.
 * Factory prepares NEW empty writers/family/catalog/pumps, init, submits local
 * definitions via pumps. ACTUAL producers enabled only after their SYNCED.
 * query/deletion optional; caller supplies verified inventory/provider hooks. */
bool storage_engine_init(StorageEngine *,StorageFileCatalog *,StoragePump **,unsigned,
 StorageQuery *,StorageDelete *,StorageEpoch *,StorageBootstrapPlan **,
 const uint32_t *,const uint64_t *,const StorageEpochHooks *,
 uint64_t created_us,uint64_t max_file_bytes,uint64_t max_file_age_us);
/* Each call at most ONE backend operation (adapter operation may perform FatFs
 * seek+read). At high water/audit, writer wins. Otherwise background gets1/16
 * calls; never holds queue lock while file IO. No hard IO-latency claim.
 * rotation_due is owner-only advisory: capture controller must flush/quiesce
 * producers before request_rotation, preserving explicit loss/summary facts. */
/* Explicit cold path: config/capture remain zero in RAM, never definitions.
 * Await mode pumps metadata only; no query/delete/catalog/rotation work.
 * Bind validates ALL local exact observed-SYNCED definitions before mutation.
 * Caller supplies identities from real START/ACTIVE, not a test seed. */
bool storage_engine_init_unbound(StorageEngine *,StorageFileCatalog *,StoragePump **,unsigned,
 StorageEpoch *,StorageBootstrapPlan **,const StorageEpochHooks *,uint64_t,uint64_t,uint64_t);
StorageResult storage_engine_bind_capture(StorageEngine *,uint32_t config,uint64_t capture);
void storage_engine_step(StorageEngine *,uint64_t monotonic_us);
StorageResult storage_engine_request_rotation(StorageEngine *);
#endif
