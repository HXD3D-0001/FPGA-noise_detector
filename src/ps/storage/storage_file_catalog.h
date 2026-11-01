#ifndef LFN_STORAGE_FILE_CATALOG_H
#define LFN_STORAGE_FILE_CATALOG_H
#include "storage_family_queue.h"
#include "storage_family_index.h"
#include "storage_service.h"
/* Explicit live-owner opt-in, NOT connected to capture/storage RTOS tasks.
 * All APIs belong to the sole storage owner. No filesystem close/delete,
 * allocator restart, rotation or media formatting. Hidden handle/path aliases
 * must be excluded by the caller. All descriptors refer to NEW EMPTY files. */
enum { STORAGE_CATALOG_FRAME=256,STORAGE_CATALOG_NAME=64 };
typedef enum { CATALOG_MONITOR=1,CATALOG_RAW,CATALOG_EVENT } StorageCatalogRole;
typedef enum { CATALOG_BASE=0,CATALOG_EVENT_TREE=1 } StorageCatalogLocation;
typedef struct {
 uint64_t file_id,created_us;StorageCatalogRole role;bool created_known;
 char name[STORAGE_CATALOG_NAME]; /* ASCII basename, canonical zero padding. */
 StorageCatalogLocation location;uint64_t ordinal;
} StorageFileDescriptor;
typedef enum { CATALOG_PENDING,CATALOG_OPEN,CATALOG_SEALED } StorageCatalogState;
typedef struct {
 StorageFileDescriptor descriptor;StorageCatalogState state;
 uint64_t revision,file_bytes,family_prefix,records,first_seq,last_seq,sealed_us,admission_cutoff;
 uint32_t file_crc,definitions;bool sealed_known;
} StorageCatalogEntry;
/* Persistent caller-owned workspace (~600KiB), never capture/ISR/stack. Fresh
 * registry rebuild proves this file's OWN definitions and ordered references.
 * Physical definition sequences are retained for exact dependency checking. */
typedef struct {
 StorageMetadataRegistry registry;MetadataJson scratch;uint8_t record[RECORD_MAX_BYTES];
 uint64_t definitions[STORAGE_FAMILY_ENTRIES];unsigned count;
} StorageFileVerifier;
typedef enum { CATALOG_SIZE,CATALOG_OPEN_WRITE,CATALOG_OPEN_SYNC,CATALOG_OPEN_PUBLISH,
 CATALOG_IDLE,CATALOG_DRAIN,CATALOG_FREEZE,CATALOG_FILE_SIZE,CATALOG_HEADER,
 CATALOG_BODY,CATALOG_ASSOCIATION,CATALOG_LEDGER_REST,CATALOG_SEAL_WRITE,CATALOG_SEAL_SYNC,CATALOG_FAULT } StorageCatalogPhase;
typedef struct {
 StorageBackend backend;StorageFamilyQueue *owner;StorageService *services[STORAGE_FAMILY_FILES];
 StorageCatalogEntry files[STORAGE_FAMILY_FILES];StorageFileVerifier *verifier;StorageFamilyIndex index;
 /* Historical seals have no service pointer: never operate an old ID on the
  * replacement writer. Active + history is limited to eight disk identities. */
 StorageCatalogEntry history[STORAGE_FAMILY_FILES-1];unsigned history_count;bool replacing;
 uint64_t catalog_id,bytes,revision,last_now,offset;unsigned count,active;
 size_t used,total;bool have_time,index_started,epoch_handoff;StorageError error;StorageCatalogPhase phase;
 uint8_t frame[STORAGE_CATALOG_FRAME];
} StorageFileCatalog;
/* Requires pristine streaming family/queues/services; atomically pauses ALL
 * admissions. Nonempty manifest faults without truncate. Each OPEN must sync
 * before that queue is resumed. BUSY init leaves all queues unchanged. */
StorageResult storage_file_catalog_init(StorageFileCatalog *,StorageFamilyQueue *,StorageService **,
 const StorageFileDescriptor *,unsigned,const StorageBackend *,uint64_t catalog_id,StorageFileVerifier *);
/* Caller stops/flushes producers and seals service first. Request does no IO;
 * step drains audit/outstanding jobs, closes queue admission permanently,
 * freezes family file, snapshots writer prefix and durable terminal ledger.
 * Then bounded reads rebuild local schema and check EVERY record's exact
 * durable SFL1 association/dependencies. One forward ledger pass per file, no target throughput claim.
 * SEALED published ONLY after seal manifest sync. Failure never grants proof. */
StorageResult storage_file_catalog_request_seal(StorageFileCatalog *,uint64_t,uint64_t sealed_us,bool known);
void storage_file_catalog_step(StorageFileCatalog *,uint64_t now_us);
/* Output unchanged until durable state. OPEN can be polled while sealing. */
StorageResult storage_file_catalog_get(const StorageFileCatalog *,uint64_t,StorageCatalogEntry *);
StorageResult storage_file_catalog_replace_event(StorageFileCatalog *,unsigned,const StorageFileDescriptor *);
/* Resolve from the actual catalog pathname, never the live epoch in RAM. */
bool storage_file_catalog_resolve(const char *catalog_path,const StorageFileDescriptor *,char *out,size_t capacity);
/* Format/semantics decoder for one SFC1/v1 frame. NOT lifecycle replay,
 * post-restart verification, writable resume or permission to delete a file. */
bool storage_file_catalog_decode(const uint8_t *,size_t,uint64_t session,uint64_t catalog_id,
 uint64_t ledger_id,StorageCatalogEntry *);
#endif
