#ifndef LFN_STORAGE_DELETE_H
#define LFN_STORAGE_DELETE_H
#include "storage_retention.h"
enum { STORAGE_DELETE_FRAME=256 };
typedef struct {
 void *context;
 /* Pure, atomic inventory claim excludes new pins and verifies current closed,
  * revalidated exact identity. BUSY while referenced. Hold claim on any fault. */
 StorageResult (*claim)(void *,const StorageRetentionFile *);
 StorageIoResult (*remove)(void *,const StorageRetentionFile *,bool *already_missing);
 StorageIoResult (*sync_media)(void *);
 void (*release)(void *,const StorageRetentionFile *);
} StorageDeleteHooks;
typedef enum { DELETE_SIZE,DELETE_IDLE,DELETE_CLAIM,DELETE_INTENT_WRITE,DELETE_INTENT_SYNC,DELETE_UNLINK,DELETE_MEDIA_SYNC,DELETE_DONE_WRITE,DELETE_DONE_SYNC,DELETE_RELEASE,DELETE_FAULT } StorageDeleteState;
typedef struct {
 StorageBackend journal;StorageDeleteHooks hooks;uint64_t owner_session,journal_id,bytes,next_operation;
 StorageRetentionFile victim;uint64_t operation;StorageDeleteState state;bool held,missing;
 uint8_t frame[STORAGE_DELETE_FRAME];
} StorageDelete;
typedef struct {uint64_t owner_session,journal_id,operation;bool complete;StorageRetentionFile file;} StorageDeleteRecord;
/* New EMPTY uniquely identified journal only, no resume at a scanned offset.
 * Both INTENT and COMPLETE are synced. No unlink before durable intent.
 * Remove missing may be idempotent only after exact claim succeeds.
 * Fault keeps deletion exclusion until explicit owner recovery; never retry
 * a stale victim or infer physical free clusters from logical bytes. */
bool storage_delete_init(StorageDelete *,const StorageBackend *,uint64_t session,uint64_t journal_id,const StorageDeleteHooks *);
StorageResult storage_delete_request(StorageDelete *,const StorageRetentionFile *);
void storage_delete_step(StorageDelete *);
bool storage_delete_decode(const uint8_t *,size_t,StorageDeleteRecord *);
typedef enum { DELETE_SCAN_SIZE,DELETE_SCAN_READ,DELETE_SCAN_DONE,DELETE_SCAN_TAIL,DELETE_SCAN_IO } StorageDeleteScanState;
typedef struct {
 StorageBackend backend;uint64_t session,journal_id,size,prefix,last_operation;
 StorageDeleteRecord pending,found;uint64_t target_session,target_file;
 bool have_pending,have_found;StorageDeleteScanState state;uint8_t frame[256];
} StorageDeleteScan;
/* Sequential first-valid-prefix scan. Recorded intent/complete is historical
 * evidence only, not permission to unlink. Cold recovery must inspect current
 * presence/identity and revalidate data before any new claim. A completed
 * tombstone plus confirmed absent file permits EXPIRED_BY_RETENTION response. */
bool storage_delete_scan_init(StorageDeleteScan *,const StorageBackend *,uint64_t session,uint64_t journal_id,uint64_t target_session,uint64_t target_file);
void storage_delete_scan_step(StorageDeleteScan *);
#endif
