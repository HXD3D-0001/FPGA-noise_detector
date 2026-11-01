#ifndef LFN_STORAGE_FAMILY_INDEX_H
#define LFN_STORAGE_FAMILY_INDEX_H
#include "storage_family.h"
/* SFL1 ledger itself is the persistent append-only index, no second index file
 * and no changed wire format. Sequential O(records), bounded RAM/IO per step.
 * Single storage owner schedules queries after write/sync work. Read only.
 * A structurally valid association is NOT data/dependency verification. */
typedef enum { FAMILY_INDEX_SIZE,FAMILY_INDEX_RESERVE,FAMILY_INDEX_TERMINAL,
 FAMILY_INDEX_MATCH,FAMILY_INDEX_NOT_FOUND,FAMILY_INDEX_TAIL,FAMILY_INDEX_IO,
 FAMILY_INDEX_CANCELLED } StorageFamilyIndexState;
typedef enum { FAMILY_INDEX_DURABLE_SNAPSHOT,FAMILY_INDEX_OBSERVED_HISTORY } StorageFamilyIndexScope;
typedef struct {
 StorageBackend ledger;uint64_t session_id,ledger_id,limit,target,prefix,last_seq;
 uint64_t files[STORAGE_FAMILY_FILES],ends[STORAGE_FAMILY_FILES];unsigned file_count,file;
 StorageFamilyEntry pending,match;StorageFamilyIndexState state;StorageFamilyIndexScope scope;
 uint8_t frame[STORAGE_IO_CHUNK];size_t cache_used,cache_bytes;
} StorageFamilyIndex;
/* committed_bytes must be an independently proven complete terminal-sync
 * prefix (live owner uses family.durable_bytes), never ledger_bytes or size.
 * Does NOT restore allocator/receipt state or authorize same-session restart. */
bool storage_family_index_init(StorageFamilyIndex *,const StorageBackend *,uint64_t session,
 uint64_t ledger,uint64_t committed_bytes,uint64_t sequence);
/* Historical read-evidence only: declared prefix is an UNVERIFIED readable
 * bound, not a known sync point. Results never become live saved receipts. */
bool storage_family_index_init_observed(StorageFamilyIndex *,const StorageBackend *,uint64_t,uint64_t,uint64_t,uint64_t);
/* Continue sorted targets without rescanning prior frames. drain reads the
 * rest of the snapshot for end/absence checks, never fabricates a sequence. */
bool storage_family_index_next(StorageFamilyIndex *,uint64_t next_sequence);
bool storage_family_index_drain(StorageFamilyIndex *);
void storage_family_index_step(StorageFamilyIndex *);
void storage_family_index_cancel(StorageFamilyIndex *);
/* Output unchanged on BUSY/not-found/error. Saved match state is always
 * REPLAY_UNVERIFIED. FAILED association has no successful-data claim. */
StorageResult storage_family_index_result(const StorageFamilyIndex *,StorageFamilyEntry *);
/* Verify only this exact record/location/schema, NOT its dependencies. Return
 * OK never upgrades match.state to full verified or resurrects a live receipt.
 * Caller supplies stable full record bytes (<=64KiB), outside capture/ISR. */
StorageResult storage_family_index_check_record(const StorageFamilyIndex *,uint64_t file,uint64_t offset,
 const uint8_t *,size_t,bool (*validate)(void *,const RecordIdentity *,const uint8_t *,size_t),void *);
#endif
