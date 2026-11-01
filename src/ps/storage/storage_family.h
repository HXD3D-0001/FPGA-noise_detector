#ifndef LFN_STORAGE_FAMILY_H
#define LFN_STORAGE_FAMILY_H
#include "storage_writer.h"
/* E3 core. ONE storage owner, no ISR/capture callers. Exclusive ownership:
 * queue/service access only through the explicit family_queue adapter.
 * Existing capture/storage task is deliberately not switched to this core. */
enum { STORAGE_FAMILY_FILES=8,STORAGE_FAMILY_ENTRIES=64,STORAGE_FAMILY_FRAME=256,
 /* Three admission slots stay reserved for an EVENT-roll bootstrap closure
  * (SESSION/CONFIG/CAPTURE) even when the data backlog is saturated. */
 STORAGE_FAMILY_DEFINITION_RESERVE=3,
 STORAGE_FAMILY_ADMISSION_LIMIT=STORAGE_FAMILY_ENTRIES-STORAGE_FAMILY_DEFINITION_RESERVE };
typedef enum { FAMILY_RESERVED,FAMILY_DATA_SYNCED,FAMILY_SAVED,FAMILY_FAILED,
 FAMILY_REPLAY_UNVERIFIED,FAMILY_REPLAY_VERIFIED } StorageFamilyState;
typedef struct {
 RecordIdentity identity;uint64_t file_id,offset,total_bytes;
 uint32_t record_crc;unsigned dependency_count;uint64_t dependencies[STORAGE_DEPENDENCIES];
 StorageFamilyState state;StorageError error;bool reservation_durable,terminal_durable;
} StorageFamilyEntry;
typedef enum { FAMILY_OPENING,FAMILY_READY,FAMILY_RESERVE_WRITE,FAMILY_RESERVE_SYNC,
 FAMILY_APPEND,FAMILY_DATA,FAMILY_TERMINAL_WRITE,FAMILY_TERMINAL_SYNC,FAMILY_FAULT } StorageFamilyPhase;
typedef struct {
 StorageBackend ledger;uint64_t session_id,ledger_id,next_seq,ledger_bytes,last_now;
 uint64_t durable_bytes,total_records,retired_records;bool indexed;uint8_t closed_files;
 StorageWriter *files[STORAGE_FAMILY_FILES];unsigned file_count,count,active_file;
 StorageFamilyEntry entries[STORAGE_FAMILY_ENTRIES];StorageFamilyPhase phase;
 StorageError error;StorageTicket active_ticket;bool have_ticket,have_time,reserved_mode;
 uint8_t frame[STORAGE_FAMILY_FRAME],record[RECORD_MAX_BYTES];
} StorageFamily;
/* New EMPTY ledger and EMPTY READY writers only. IDs/seeds must be unique and
 * supplied by the controller. No same-session restart/allocator restoration;
 * that requires boot journal and complete file inventory in the later module.
 * Same write callback/context aliases rejected. Controller must also exclude
 * hidden path/handle aliases. ledger uses size/write/sync; never truncates it. */
bool storage_family_init(StorageFamily *,const StorageBackend *,uint64_t session_id,
 uint64_t ledger_id,uint64_t next_seq);
bool storage_family_attach(StorageFamily *,StorageWriter *);
/* Copies full framed record, validates each file's OWN schema gate. req local
 * tickets prohibited; dependency sequences must be exact FAMILY_SAVED entries.
 * BUSY/CAPACITY/invalid/dependency refusals allocate NO sequence (unlike E2a).
 * Successful submit.ticket is zero: query by record_seq, not writer poll.
 * Accepted sequence is never reused, even when IO fails. No dependency implies
 * automatic metadata endorsement in another file; repeat local definitions. */
StorageSubmit storage_family_submit(StorageFamily *,uint64_t file_id,const StorageRequest *,
 const uint64_t *dependencies,unsigned dependency_count);
/* Queue-owner mode, selected only before first transaction. Shared admission
 * allocator reserves globally; dispatches must arrive in increasing order.
 * No auto allocation after this mode is selected. Capacity holes allowed. */
bool storage_family_use_reserved(StorageFamily *);
StorageSubmit storage_family_append_reserved(StorageFamily *,uint64_t file_id,const StorageRequest *,
 const uint64_t *dependencies,unsigned dependency_count,uint64_t sequence);
/* One <=4KiB IO call per step, monotonic time. Durable reservation -> actual
 * writer sync -> durable terminal association. Journal failure freezes owner.
 * Writer tickets ACKed only after durable terminal. Default64 entries retained;
 * explicit indexed mode may retire handed-off DATA/FAULT, never definitions. */
void storage_family_step(StorageFamily *,uint64_t now_us);
StorageResult storage_family_query(const StorageFamily *,uint64_t sequence,StorageFamilyEntry *);
/* Explicit live-stream mode. Retire only terminal durable DATA/FAULT entries,
 * never definitions, active transactions or retained dependents. Caller first
 * hands the exact outcome to its public receipt owner. No IO/truncate here.
 * Disk index uses durable_bytes, not unsynced ledger_bytes. */
/* Sole owner: permanently freeze a drained file before certification. Other
 * file transactions may continue. Does not close a filesystem handle. */
StorageResult storage_family_freeze_file(StorageFamily *,uint64_t file_id);
bool storage_family_enable_indexed(StorageFamily *);
StorageResult storage_family_retire(StorageFamily *,uint64_t sequence);
/* Format-only SFL1 decoder shared with streaming disk queries; does not prove
 * referenced data or dependency persistence by itself. */
bool storage_family_frame_decode(const uint8_t *,StorageFamilyEntry *,uint64_t,uint64_t,unsigned *);
/* Read-only first-prefix replay. No resume, repair, truncate, writer receipt
 * resurrection or writable seed. Restored commits remain UNVERIFIED until
 * caller reads the exact data record and confirms framing/CRC/OWN schema.
 * Full record verification is storage-owner work, never capture work. */
typedef enum { FAMILY_SCAN_SIZE,FAMILY_SCAN_FRAME,FAMILY_SCAN_DONE,FAMILY_SCAN_TAIL,
 FAMILY_SCAN_CAPACITY,FAMILY_SCAN_IO } StorageFamilyScan;
typedef struct {
 StorageBackend ledger;uint64_t session_id,ledger_id,size,prefix,last_sequence;
 unsigned count;StorageFamilyEntry entries[STORAGE_FAMILY_ENTRIES];
 StorageFamilyScan state;uint8_t frame[STORAGE_FAMILY_FRAME];
} StorageFamilyReplay;
bool storage_family_replay_init(StorageFamilyReplay *,const StorageBackend *,uint64_t session_id,uint64_t ledger_id);
void storage_family_replay_step(StorageFamilyReplay *);
StorageResult storage_family_replay_query(const StorageFamilyReplay *,uint64_t sequence,StorageFamilyEntry *);
StorageResult storage_family_replay_confirm(StorageFamilyReplay *,uint64_t sequence,uint64_t file_id,uint64_t offset,
 const uint8_t *,size_t,bool (*validate)(void *,const RecordIdentity *,const uint8_t *,size_t),void *);
#endif
