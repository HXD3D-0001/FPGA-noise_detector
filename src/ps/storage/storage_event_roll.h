#ifndef LFN_STORAGE_EVENT_ROLL_H
#define LFN_STORAGE_EVENT_ROLL_H
#include "storage_factory.h"
#include "storage_file_handles.h"
#include "storage_namespace.h"
/* Sole storage owner, EVENT producers quiescent only. The caller continues
 * feeding MON/PSD; step gives those pumps half the scheduler calls. A failed
 * transaction retains its write lease and files for diagnosis. No deletion. */
typedef struct {
 void *context;
 StorageIoResult (*mkdir)(void *,const char *path); /* existing directory OK */
 StorageResult (*create)(void *,StorageFileHandles *,unsigned slot); /* CREATE_NEW */
 StorageIoResult (*sync_media)(void *,StorageFileHandles *);
 /* Optional D3 observer reinstallation; old callbacks are NOT retained across
  * file identities. Runs before new definitions can be admitted. */
 bool (*observer)(void *,StorageService *,uint64_t file_id);
} StorageEventRollOps;
typedef enum {
 EVENT_ROLL_IDLE,EVENT_ROLL_LEASE,EVENT_ROLL_DRAIN,EVENT_ROLL_SEAL,
 EVENT_ROLL_COPY,EVENT_ROLL_RELEASE,EVENT_ROLL_ACK,EVENT_ROLL_CLOSE,
 EVENT_ROLL_REPLACE_HANDLE,EVENT_ROLL_MKDIR,EVENT_ROLL_CREATE,
 EVENT_ROLL_SYNC_FILE,EVENT_ROLL_SYNC_MEDIA,EVENT_ROLL_WRITER,
 EVENT_ROLL_BIND,EVENT_ROLL_OPEN,EVENT_ROLL_BOOTSTRAP,
 EVENT_ROLL_DONE,EVENT_ROLL_FAILED
} StorageEventRollState;
typedef struct {
 StorageFactory *factory;StorageFileHandles *handles;StorageNamespace *space;
 StorageEventRollOps ops;StorageFileDescriptor next;StorageHandleSpec spec;
 StorageBootstrapWriter bootstrap;StorageEventRollState state,failed_at;
 StorageResult error;StorageError backend_error;uint64_t old_file_id,lease,last_now,steps;
 unsigned file,handle,release_slot,dir_cursor,round_robin;bool have_time;
 uint64_t writer_generation[STORAGE_RECEIPTS];
 char directory[STORAGE_HANDLE_PATH];
} StorageEventRoll;
/* Fresh or DONE only. ordinal is session-global, already consumed by caller.
 * Path is derived from actual catalog path. Count+history <=8; capacity failure
 * occurs before closing admissions. No auto ordinal reset, allocation or retry. */
StorageResult storage_event_roll_request(StorageEventRoll *,StorageFactory *,StorageFileHandles *,
 StorageNamespace *,const StorageEventRollOps *,unsigned file,unsigned handle,
 const char *catalog_path,uint64_t ordinal,uint64_t created_us);
/* At most one backend operation per call. While active this replaces engine /
 * lifecycle stepping for this group; route consumption remains caller-owned. */
void storage_event_roll_step(StorageEventRoll *,uint64_t now_us);
bool storage_event_roll_active(const StorageEventRoll *);
StorageResult storage_event_roll_status(const StorageEventRoll *,uint64_t *new_file_id);
uint64_t storage_event_roll_file_id(uint64_t session,uint64_t ordinal);
#endif
