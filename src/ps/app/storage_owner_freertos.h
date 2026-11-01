#ifndef LFN_STORAGE_OWNER_FREERTOS_H
#define LFN_STORAGE_OWNER_FREERTOS_H
#include "storage_owner.h"
#include "../storage/storage_namespace.h"
#include "../storage/fatfs_file_port.h"
#include "../storage/fatfs_namespace_port.h"
#include "../storage/fatfs_provision_port.h"
/* Real-media glue for the multi-file owner. It owns only the FatFs adapter
 * objects and the mount/unmount sequencing; every storage transition stays in
 * the owner/module state machines. The caller supplies the task-owned volume
 * object and the capture-owner mailbox ports. No malloc, no task creation and
 * no hidden retry: a failed mount leaves the structures for explicit recovery.
 *
 * Mount order is constrained by the adapters themselves: the namespace guard
 * must bind to an UNMOUNTED volume, while the file port requires a MOUNTED one.
 * A real mount/unmount also needs the namespace write lease, so this glue takes
 * a short temporary lease only around f_mount; all later leases stay with
 * provision/lifecycle as before. */
typedef struct {
 LfnFatFsVolume *volume;StorageOwner *owner;
 unsigned stage;LfnFatFsFilePort files;LfnFatFsNamespacePort guard;LfnFatFsProvisionPort provision;
 StorageFileOps file_ops;StorageProvisionOps provision_ops;StorageOwnerPorts ports;
} StorageOwnerFreertos;
typedef struct {
 LfnFatFsVolume *volume;const char *drive;const char *root;
 const LfnFatFsNamespaceName *names;unsigned name_count;
 StorageIoResult (*media_sync)(void *);void *media_context;
 bool (*is_owner)(void *);void *owner_context;
 bool (*producer_stopped)(void *);void *proof_context;
 StorageColdCapturePort cold;StorageRotationCapturePort rotation;
} StorageOwnerFreertosConfig;
/* Init volume + owner namespace, bind the guard, mount once, build both ports,
 * then complete owner assembly (attach_ports). Called from the storage task
 * only; is_owner must identify that task and producer_stopped must be the real
 * source-off proof. Returns false on the first failure (explicit recovery). */
bool lfn_storage_owner_freertos_mount(StorageOwnerFreertos *,StorageOwner *,const StorageOwnerFreertosConfig *,const StorageOwnerConfig *);
/* Only after the owner reached STOPPED/FAILED with no opened handle: takes the
 * temporary namespace lease, unmounts, releases it. Never closes handles. */
StorageIoResult lfn_storage_owner_freertos_unmount(StorageOwnerFreertos *);
bool lfn_storage_owner_freertos_mounted(const StorageOwnerFreertos *);
#endif