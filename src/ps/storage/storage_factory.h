#ifndef LFN_STORAGE_FACTORY_H
#define LFN_STORAGE_FACTORY_H
#include "storage_binding.h"
#include "storage_engine.h"
typedef struct {
 StorageBackend raw;StorageFileDescriptor descriptor;
 StorageBinding *binding;StorageWriter *writer;StorageQueue *queue;
 StorageService *service;StoragePump *pump;StorageBootstrapPlan *plan;
 uint32_t config_id;uint64_t capture_id;
} StorageFactoryFile;
typedef struct {
 uint64_t session_id,next_sequence,ledger_id,catalog_id,sync_us,created_us,max_file_bytes,max_file_age_us;
 unsigned count;StorageFactoryFile files[STORAGE_FAMILY_FILES];
 StorageBackend ledger,manifest;uint8_t *arena;StorageQueuePool *pool;
 StorageFamily *family;StorageFamilyQueue *owner;StorageFileCatalog *catalog;
 StorageFileVerifier *verifier;StorageEpoch *epoch;StorageEngine *engine;
 StorageQueueLocks locks;StorageEpochHooks hooks;
 void *owner_context;bool (*is_owner)(void *);
 /* Explicit cold path: ALL files config/capture must be zero. SESSION and
  * START-derived CONFIG/CAPTURE are synced separately, then bind first. */
 bool await_first_capture;
} StorageFactoryConfig;
typedef enum { FACTORY_CHECK_EMPTY,FACTORY_WRITERS,FACTORY_LEDGER,FACTORY_BIND,FACTORY_CATALOG,FACTORY_READY,FACTORY_BOOTSTRAP,FACTORY_FAILED } StorageFactoryState;
typedef struct {
 StorageFactoryConfig config;StorageFactoryState state;StorageResult error;
 unsigned cursor;bool have_time;uint64_t last_now;
 StorageBootstrapWriter bootstrap[STORAGE_FAMILY_FILES];
} StorageFactory;
/* Sole owner, fresh exclusive persistent objects and already-open NEW files.
 * No allocation/mount/create/format/SD inference. Raw backends must not hide
 * path aliases. Validate all object regions and backend/ID aliases before IO.
 * Once init succeeds resources are owned by factory, including on failure.
 * Never reinit live resources or restore a seed from historical disk scans. */
/* Same preflight as init, no mutation or IO. Does not prove media empty. */
bool storage_factory_validate(StorageFactory *,const StorageFactoryConfig *);
bool storage_factory_init(StorageFactory *,const StorageFactoryConfig *);
/* One backend operation per step. All files checked EMPTY before any OPEN
 * writes. READY means owner assembled, NOT metadata/measurement ready. Cold
 * start must publish real local definitions and wait exact receipts separately. */
void storage_factory_step(StorageFactory *,uint64_t monotonic_us);
/* Fresh READY services only. Deep plans supplied by old epoch; these plans
 * remain stable while replaying. After all definitions SYNCED returns READY.
 * Caller keeps producers quiescent throughout, then explicitly reconnects.
 * Failure retains all files/receipts; no auto close/delete/rollback/retry. */
bool storage_factory_bootstrap(StorageFactory *,const StorageBootstrapPlan *const *);
/* Sole owner, pure, all-or-none local definitions readiness. Does not enable
 * producers or route consumption; lifecycle_enable remains mandatory. */
StorageResult storage_factory_bind_first_capture(StorageFactory *,uint32_t config,uint64_t capture);
#endif
