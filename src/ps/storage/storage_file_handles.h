#ifndef LFN_STORAGE_FILE_HANDLES_H
#define LFN_STORAGE_FILE_HANDLES_H
#include "storage_query.h"
#include "storage_delete.h"
#include "storage_archive_verify.h"
enum { STORAGE_HANDLES=16,STORAGE_FILE_PINS=32,STORAGE_HANDLE_PATH=192 };
typedef enum { HANDLE_DATA,HANDLE_AUXILIARY } StorageHandleKind;
typedef struct {
 uint64_t session_id,file_id,catalog_id,ledger_id,order_key;
 StorageHandleKind kind;StorageRetentionFamily family;char path[STORAGE_HANDLE_PATH];
} StorageHandleSpec;
typedef struct {
 void *context;bool (*is_owner)(void *);
 /* One filesystem adapter operation per callback. open is READ ONLY and
  * existing-only. A failed close MUST retain handle and exclusion. */
 StorageIoResult (*open)(void *,unsigned slot,const char *,StorageBackend *);
 StorageIoResult (*close)(void *,unsigned slot);
 StorageIoResult (*remove)(void *,const char *,bool *missing);
 StorageIoResult (*sync_media)(void *);
} StorageFileOps;
typedef struct {uint64_t token;unsigned file;bool used,query,failed_release;} StorageFilePin;
typedef struct {
 StorageHandleSpec spec;StorageBackend backend;StorageCatalogEntry seal;
 unsigned references;bool used,opened,active,sealed,verified,closing,claimed,removed,expired;
} StorageHandle;
typedef struct {
 StorageFileOps ops;StorageHandle files[STORAGE_HANDLES];StorageFilePin pins[STORAGE_FILE_PINS];
 uint64_t next_pin;unsigned deletion_slot;
} StorageFileHandles;
/* Sole storage-owner only, no locks/IO in registration or pin_existing.
 * Cache, NOT complete directory inventory: uncached IDs return NOT_READY.
 * Slots may be forgotten only closed/unpinned/inactive and not claimed/closing.
 * Do not reinit while any pin/handle/client exists. SFN8.3 canonical relative
 * paths are deliberate portable layout policy, not guessed board parameters. */
bool storage_file_handles_init(StorageFileHandles *,const StorageFileOps *);
StorageResult storage_file_handles_register(StorageFileHandles *,const StorageHandleSpec *,unsigned *slot);
StorageResult storage_file_handles_attach(StorageFileHandles *,unsigned,const StorageBackend *already_open_active);
StorageResult storage_file_handles_seal(StorageFileHandles *,unsigned,const StorageCatalogEntry *);
/* Freeze active ledger/catalog only after the matching live catalog has sealed
 * every data file. Does not freeze arbitrary boot/delete journals. */
StorageResult storage_file_handles_freeze_aux(StorageFileHandles *,unsigned,const StorageFileCatalog *);
StorageResult storage_file_handles_open_read(StorageFileHandles *,unsigned);
StorageResult storage_file_handles_pin(StorageFileHandles *,unsigned,uint64_t *);
/* Explicit owner recovery for a query release whose actual close failed.
 * Token is retained in pins[] for diagnosis; never clears a live successful pin. */
StorageIoResult storage_file_handles_retry_query_release(StorageFileHandles *,uint64_t);
StorageIoResult storage_file_handles_unpin(StorageFileHandles *,uint64_t);
/* Close rejects every pin and fences new ones immediately, even on BUSY or
 * failed close. Finish_close clears closing after successful actual close.
 * Epoch owner can keep fence until whole epoch is closed before finishing. */
StorageResult storage_file_handles_close(StorageFileHandles *,unsigned);
/* Sole owner atomic RAM transition for a live fully SEALED epoch. Slots in
 * catalog data order, then ledger,catalog. Pins may remain: fence ALL slots
 * before any close, preserve exact old pins, never grant verified eligibility.
 * finish_epoch requires every actual close/reference completed, then clears
 * the group fences without IO. No boot/delete journal or historical claim. */
StorageResult storage_file_handles_freeze_epoch(StorageFileHandles *,const StorageFileCatalog *,const unsigned *slots);
StorageResult storage_file_handles_finish_epoch(StorageFileHandles *,const StorageFileCatalog *,const unsigned *slots);
StorageResult storage_file_handles_finish_close(StorageFileHandles *,unsigned);
StorageResult storage_file_handles_forget(StorageFileHandles *,unsigned);
/* Accept current revalidation ONLY when both data AND ledger are pinned in
 * this cache, exact backends match and seal fields are unchanged. Historical
 * directory seal alone cannot grant deletion eligibility. */
StorageResult storage_file_handles_revalidated(StorageFileHandles *,unsigned,const StorageArchiveVerifier *);
StorageResult storage_file_handles_inventory(StorageFileHandles *,unsigned,StorageRetentionFile *);
StorageQueryFiles storage_file_handles_query(StorageFileHandles *);
StorageDeleteHooks storage_file_handles_delete(StorageFileHandles *);
#endif
