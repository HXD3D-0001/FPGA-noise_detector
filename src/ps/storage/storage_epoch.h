#ifndef LFN_STORAGE_EPOCH_H
#define LFN_STORAGE_EPOCH_H
#include "storage_bootstrap.h"
#include "storage_file_catalog.h"
typedef struct {
 void *context;
 /* Pure proof: producers stopped/flush complete, no caller inside admission. */
 bool (*quiescent)(void *);
 /* One storage-owner close operation; BUSY if any external query/event pin.
  * Return OK only after actual close. Failure keeps old epoch protected. */
 StorageResult (*close)(void *,uint64_t file_id);
} StorageEpochHooks;
typedef enum { EPOCH_DRAIN,EPOCH_SEAL,EPOCH_COPY,EPOCH_RELEASE,EPOCH_ACK_BARRIER,EPOCH_CLOSE,EPOCH_READY,EPOCH_FAILED } StorageEpochState;
typedef struct {
 uint64_t session_id,next_sequence,catalog_id,ledger_id,files[STORAGE_FAMILY_FILES];
 unsigned count;bool valid,consumed;
} StorageEpochToken;
typedef struct {
 StorageFileCatalog *catalog;StorageBootstrapPlan *plans[STORAGE_FAMILY_FILES];
 uint32_t config[STORAGE_FAMILY_FILES];uint64_t capture[STORAGE_FAMILY_FILES];
 StorageEpochHooks hooks;StorageEpochToken token;StorageEpochState state;
 unsigned cursor,round_robin,slot;StorageResult error;
} StorageEpoch;
/* Explicit sole-owner rotation. Caller first seals every service after flushing
 * all producer handoffs. Persistent deep-copy plans supplied for every file.
 * One IO per step. No old allocator/receipt reconstruction. External public
 * receipts must be ACKed; never silently discarded. Close callbacks guard pins.
 * On READY, old epoch may be freed, token seeds FRESH EMPTY new files/ledger/
 * catalog; bootstrap local definitions and wait DONE before producer resume.
 * Reboot must get a NEW BootJournal session, never this RAM token. */
bool storage_epoch_init(StorageEpoch *,StorageFileCatalog *,StorageBootstrapPlan **,
 const uint32_t *config,const uint64_t *capture,const StorageEpochHooks *);
void storage_epoch_step(StorageEpoch *,uint64_t now_us);
/* One-shot handoff validates different IDs, same session, strictly monotonic
 * live allocator. Caller supplies factory with returned seed; no IO here. */
bool storage_epoch_consume(StorageEpoch *,uint64_t new_catalog,uint64_t new_ledger,
 const uint64_t *new_files,unsigned count,uint64_t *session,uint64_t *seed);
#endif
