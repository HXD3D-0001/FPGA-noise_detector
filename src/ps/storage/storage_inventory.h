#ifndef LFN_STORAGE_INVENTORY_H
#define LFN_STORAGE_INVENTORY_H
#include "storage_catalog_replay.h"
#include "storage_file_handles.h"
enum { STORAGE_INVENTORY_DEPTH=8,STORAGE_INVENTORY_PATH=192 };
typedef struct {char name[64];uint64_t bytes;bool directory;} StorageDirectoryItem;
typedef struct {
 void *context;bool (*is_owner)(void *);
 /* Pure namespace read guard. Exclude directory/catalog/delete-log mutations
  * until release; existing capture data may append outside scanned sealed
  * ranges. version is nonzero and must change on EVERY namespace mutation.
  * Never make these callbacks a noop while allowing concurrent mutation. */
 StorageResult (*acquire)(void *,const char *root,uint64_t *version);
 uint64_t (*version)(void *);void (*release)(void *);
 StorageIoResult (*open_directory)(void *,unsigned depth,const char *);
 StorageIoResult (*next)(void *,unsigned depth,StorageDirectoryItem *,bool *have);
 StorageIoResult (*close_directory)(void *,unsigned depth);
 StorageIoResult (*open_file)(void *,const char *,StorageBackend *);
 StorageIoResult (*close_file)(void *);
 StorageIoResult (*stat)(void *,const char *,bool *exists,bool *directory,uint64_t *bytes);
} StorageInventoryOps;
typedef struct {
 char root[192],catalog_name[64],ledger_name[64],delete_name[64];
 /* Explicit basename->family software layout, no inferred PSD/summary family.
  * Empty family name disables that mapping. Catalog+ledger share directory. */
 char family_names[RETAIN_FAMILIES][64];
} StorageInventoryLayout;
typedef enum { INVENTORY_RUNNING,INVENTORY_PRESENT,INVENTORY_OPEN,INVENTORY_ABSENT,
 INVENTORY_EXPIRED,INVENTORY_MISSING,INVENTORY_CORRUPT,INVENTORY_INCOMPLETE,
 INVENTORY_IO,INVENTORY_CAPACITY,INVENTORY_CANCELLED } StorageInventoryStatus;
typedef struct {
 StorageInventoryStatus status;uint64_t session_id,file_id,version,directories,files,catalogs,delete_logs;
 StorageHandleSpec spec;StorageCatalogEntry recorded;char ledger_path[192];
 bool have_catalog,have_delete;StorageDeleteRecord deletion;
} StorageInventoryResult;
typedef struct {
 StorageInventoryOps ops;StorageInventoryLayout layout;StorageInventoryResult result;
 StorageCatalogReplay catalog;StorageDeleteScan deletion;StorageBackend backend;
 char paths[STORAGE_INVENTORY_DEPTH][192],file_path[192];
 unsigned depth,phase,metadata_kind;uint32_t data_seen[STORAGE_INVENTORY_DEPTH],declared[STORAGE_INVENTORY_DEPTH];bool snapshot,file_open,dirs[STORAGE_INVENTORY_DEPTH],incomplete,cancel;
 StorageInventoryStatus desired;uint8_t first[256];
} StorageInventory;
/* Persistent constant RAM regardless of number of directories/files. One
 * directory entry or one size/read<=256B/stat/close per step. Root explicitly
 * owned by this software layout; never scan arbitrary unrelated volumes.
 * Captured historical seals remain UNVERIFIED. No filesystem write/delete,
 * allocator resume, media/controller choice, or promotion of old receipts. */
bool storage_inventory_init(StorageInventory *,const StorageInventoryOps *,const StorageInventoryLayout *,uint64_t session,uint64_t file);
void storage_inventory_step(StorageInventory *);
void storage_inventory_cancel(StorageInventory *);
/* Failed close keeps handles+namespace guard held; retry cleanup explicitly.
 * No new scan/reinit until all acquired resources have actually closed. */
bool storage_inventory_retry_cleanup(StorageInventory *);
bool storage_inventory_ready(const StorageInventory *);
StorageResult storage_inventory_load_handles(const StorageInventory *,StorageFileHandles *);
typedef struct {StorageInventory *inventory;StorageFileHandles *handles;StorageQueryFiles cached;} StorageInventoryQuery;
/* Scanner runs to terminal separately (no orphan job on query cancellation).
 * Lookup result bound to exact request and unchanged namespace version.
 * Cache loading does not certify data/ledger. Unknown/incomplete -> NOT_READY. */
bool storage_inventory_query_init(StorageInventoryQuery *,StorageInventory *,StorageFileHandles *,StorageQueryFiles *);
#endif
