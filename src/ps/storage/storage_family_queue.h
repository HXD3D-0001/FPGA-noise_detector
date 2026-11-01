#ifndef LFN_STORAGE_FAMILY_QUEUE_H
#define LFN_STORAGE_FAMILY_QUEUE_H
#include "storage_family.h"
#include "storage_queue.h"
typedef struct StorageFamilyQueue StorageFamilyQueue;
typedef struct {StorageFamilyQueue *owner;StorageQueue *queue;unsigned file;StorageQueueTransport transport;} StorageFamilyPort;
struct StorageFamilyQueue {
 StorageFamily *family;StorageQueueSequence sequence;StorageQueueLocks locks;
 StorageFamilyPort ports[STORAGE_FAMILY_FILES];unsigned count;bool streaming;
};
/* One storage executor for ALL service/queue/family operations. Initialize
 * empty READY family with all writers already attached; then init each queue
 * and attach it here, then init services. Use the exact same locks for ALL
 * queues; distinct arenas or explicitly initialized SAME StorageQueuePool.
 * No hidden pool aliases, independent writer stepping or direct family IO.
 * No RTOS auto-switch: controller chooses this path explicitly. */
bool storage_family_queue_init(StorageFamilyQueue *,StorageFamily *,const StorageQueueLocks *);
bool storage_family_queue_attach(StorageFamilyQueue *,StorageQueue *);
/* Opt-in before first admission and after attaching all ports. DATA/FAULT
 * outcomes copied to queue receipts may retire; metadata remains pinned.
 * Internal ACK runs under common queue lock and returns its admission credit.
 * Rolling mode refuses DATA/FAULT as new dependency tickets. */
bool storage_family_queue_enable_streaming(StorageFamilyQueue *);
/* Whole sealed EVENT file teardown, after all public receipts were ACKed.
 * Rejects external dependents. Does not reopen the family slot. */
StorageResult storage_family_queue_release_file(StorageFamilyQueue *,unsigned port);
/* Each service_step drives at most ONE family IO, outside shared queue lock.
 * Admission sequence includes rejected-capacity holes; dispatch selects the
 * lowest queued sequence across all ports before starting the next transaction.
 * Default fixed mode accepts64; streaming returns DATA/FAULT admission credits.
 * No rotation, file deletion or allocator restart in this module.
 * Public queue SYNCED means data AND ledger synced, not data-sync alone. */
#endif
