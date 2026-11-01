#ifndef LFN_STORAGE_CATALOG_REPLAY_H
#define LFN_STORAGE_CATALOG_REPLAY_H
#include "storage_file_catalog.h"
/* Read-only historical SFC1 lifecycle parser. ONE owner, no writer/queue/
 * service attachment. File size is a read snapshot, NEVER a durable boundary
 * or allocator seed. Freeze backend contents while scanning; growth after
 * SIZE is excluded, shrink/read failure stops. No resync past a bad frame. */
typedef enum { CATALOG_REPLAY_SIZE,CATALOG_REPLAY_FRAME,CATALOG_REPLAY_DONE,
 CATALOG_REPLAY_TAIL,CATALOG_REPLAY_CAPACITY,CATALOG_REPLAY_IO,CATALOG_REPLAY_CANCELLED } StorageCatalogReplayState;
typedef enum { CATALOG_REPLAY_NONE,CATALOG_REPLAY_PARTIAL,CATALOG_REPLAY_FORMAT,
 CATALOG_REPLAY_REVISION,CATALOG_REPLAY_DUPLICATE_ID,CATALOG_REPLAY_DUPLICATE_NAME,
 CATALOG_REPLAY_ORDER,CATALOG_REPLAY_DESCRIPTOR,CATALOG_REPLAY_PROOF_ORDER,
 CATALOG_REPLAY_FILE_LIMIT,CATALOG_REPLAY_BACKEND,CATALOG_REPLAY_CANCEL } StorageCatalogReplayReason;
typedef enum { CATALOG_RECORDED_OPEN,CATALOG_RECORDED_SEAL_UNVERIFIED } StorageCatalogEvidence;
typedef struct {
 StorageCatalogEntry recorded;StorageCatalogEvidence evidence;
 StorageCatalogReplayState scan_state;StorageCatalogReplayReason stopped_because;
 uint64_t accepted_prefix,snapshot_bytes;
} StorageCatalogHistory;
typedef struct {
 StorageBackend backend;uint64_t session_id,catalog_id,ledger_id,size,prefix,revision;
 uint64_t last_family_prefix,last_cutoff;unsigned count;
 StorageCatalogEntry files[STORAGE_FAMILY_FILES];StorageCatalogReplayState state;
 StorageCatalogReplayReason reason;uint8_t frame[STORAGE_CATALOG_FRAME];
} StorageCatalogReplay;
/* No IO in init; read-only backend requires only size/read. Objects cannot be
 * reinitialized during a scan or while another owner uses the same buffers. */
bool storage_catalog_replay_init(StorageCatalogReplay *,const StorageBackend *,uint64_t session,uint64_t catalog_id,uint64_t ledger_id);
/* At most one size or256B read per call. Exact revision1..N; distinct OPEN
 * IDs/names; immutable OPEN->SEAL once; no reopen/reseal. Global seal ledger
 * prefix/cutoff cannot regress. Capacity never overwrites earlier entries. */
void storage_catalog_replay_step(StorageCatalogReplay *);
void storage_catalog_replay_cancel(StorageCatalogReplay *);
/* BUSY while scanning. On any terminal state, accepted-prefix entries may be
 * inspected, with scan termination included in the result. Missing -> EXPIRED
 * only for DONE, otherwise INVALID (incomplete inventory). Output unchanged
 * on error. Recorded SEALED is ALWAYS SEAL_UNVERIFIED: no data/ledger reads,
 * receipt resurrection, writable restart, truncation, close or delete rights. */
StorageResult storage_catalog_replay_get_recorded(const StorageCatalogReplay *,uint64_t file,StorageCatalogHistory *);
#endif
