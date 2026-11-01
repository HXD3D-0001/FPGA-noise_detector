#ifndef LFN_STORAGE_PROVISION_H
#define LFN_STORAGE_PROVISION_H
#include "storage_lifecycle.h"
#include "storage_file_handles.h"
enum { STORAGE_PROVISION_FILES=STORAGE_FAMILY_FILES+2 };
typedef struct {
 void *context;
 /* Pure preflight. Explicit NEW-file port, same namespace and metadata roles. */
 bool (*validate)(void *,StorageFileHandles *,StorageNamespace *,const unsigned *,unsigned);
 /* BUSY must have no effects; every other failure is terminal, may retain IO. */
 StorageResult (*create)(void *,StorageFileHandles *,unsigned);
 StorageIoResult (*sync_media)(void *,StorageFileHandles *);
} StorageProvisionOps;
typedef enum { PROVISION_WAIT_WRITE,PROVISION_CREATE,PROVISION_SYNC_FILES,PROVISION_SYNC_MEDIA,PROVISION_HANDOFF,PROVISION_ASSEMBLE,PROVISION_METADATA,PROVISION_FAILED } StorageProvisionState;
typedef struct {
 StorageFactoryConfig config;StorageProvisionOps ops;StorageFileHandles *handles;
 StorageNamespace *space;StorageFactory *factory;StorageLifecycle *lifecycle;
 unsigned slots[STORAGE_PROVISION_FILES],cursor,created;StorageProvisionState state;
 StorageResult error;StorageProvisionState failed_at;StorageIoResult io_result;
 uint64_t lease,last_now;bool have_time,have_io_result;
} StorageProvision;
/* Explicit already-mounted volume + existing epoch directory, all slots
 * registered idle. Order: data[config.count], ledger, catalog. config raw/ledger/
 * manifest must be empty; adapter supplies new actual backends after create.
 * Pure init validates object geometry using inert placeholder backends; factory
 * is not initialized or stepped until creation + file/media sync succeed.
 * All objects persistent/distinct, one executor; don't mutate specs/resources
 * or reinit while owned (including failure). Keeps production quiescent.
 * No mkdir/mount/format/boot ID allocation, no automatic rollback/close/delete. */
bool storage_provision_init(StorageProvision *,const StorageProvisionOps *,StorageFileHandles *,StorageNamespace *,StorageFactory *,StorageLifecycle *,const StorageFactoryConfig *,const unsigned *);
/* <= one CREATE_NEW, file sync, media sync, or factory backend op each step.
 * Read lease/BUSY waits before mutation. Terminal failure retains opened prefix
 * and held write token. Handoff transfers SAME token, with no unprotected gap.
 * METADATA: sole stepping ownership transfers to lifecycle; local definitions
 * must still be replayed and exactly SYNCED before explicit enable/START. */
void storage_provision_step(StorageProvision *,uint64_t monotonic_us);
#endif
