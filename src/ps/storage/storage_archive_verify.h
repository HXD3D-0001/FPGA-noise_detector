#ifndef LFN_STORAGE_ARCHIVE_VERIFY_H
#define LFN_STORAGE_ARCHIVE_VERIFY_H
#include "storage_history.h"
#include "storage_family_index.h"
#include "storage_file_catalog.h"
typedef enum { ARCHIVE_READ_DATA,ARCHIVE_ASSOCIATION,ARCHIVE_DRAIN_LEDGER,ARCHIVE_READ_REVALIDATED,ARCHIVE_BAD,ARCHIVE_CANCELLED } StorageArchiveState;
typedef struct {
 StorageHistoryReader *reader;StorageBackend ledger;StorageFamilyIndex index;StorageCatalogEntry recorded;
 uint64_t session_id,ledger_id,definitions[STORAGE_FAMILY_ENTRIES],first_sequence,last_sequence;unsigned definition_count;
 bool index_started;StorageArchiveState state;
} StorageArchiveVerifier;
/* Pin BOTH files and stable ranges before init. Historical SEALED is merely
 * the expected claim; this verifier actually reads all data/required metadata,
 * exact SFL1 associations/dependencies and remaining ledger through its bound.
 * One forward ledger pass, bounded one IO<=4KiB per step, caller-owned reader.
 * Success is current READ_REVALIDATED evidence, NEVER old STORE_SYNCED,
 * recovered allocator seed or proof of arbitrary FAT power-loss tolerance. */
bool storage_archive_verify_init(StorageArchiveVerifier *,StorageHistoryReader *,const StorageBackend *data,const StorageBackend *ledger,uint64_t session,uint64_t ledger_id,const StorageCatalogEntry *recorded);
void storage_archive_verify_step(StorageArchiveVerifier *);
void storage_archive_verify_cancel(StorageArchiveVerifier *);
#endif
