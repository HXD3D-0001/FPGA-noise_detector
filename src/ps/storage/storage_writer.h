#ifndef LFN_STORAGE_WRITER_H
#define LFN_STORAGE_WRITER_H
#include "record_codec.h"
/* Framed append/receipt core. One owner serializes all calls (including polls).
 * Producers will reach it through the later bounded storage-task mailbox;
 * never invoke these IO callbacks from the capture task or an ISR. */
enum { STORAGE_RECEIPTS=64,STORAGE_DEPENDENCIES=8,STORAGE_IO_CHUNK=4096 };
typedef enum { STORE_IO_OK,STORE_IO_NO_MEDIA,STORE_IO_NO_SPACE,STORE_IO_ERROR } StorageIoResult;
typedef enum { STORE_QUEUED,STORE_WRITTEN,STORE_SYNCED,STORE_FAILED } StorageReceiptState;
typedef enum { STORE_OK,STORE_INVALID,STORE_CAPACITY,STORE_EXHAUSTED,STORE_EXPIRED,STORE_BUSY,STORE_DEPENDENCY } StorageResult;
typedef enum { STORE_ERROR_NONE,STORE_ERROR_SHORT_WRITE,STORE_ERROR_IO,STORE_ERROR_NO_MEDIA,
 STORE_ERROR_NO_SPACE,STORE_ERROR_SHORT_READ,STORE_ERROR_CLOCK,STORE_ERROR_DEPENDENCY } StorageError;
typedef enum { SW_SCAN_SIZE,SW_SCAN_HEADER,SW_SCAN_BODY,SW_READY,SW_WRITE,SW_SYNC,
 SW_TAIL,SW_TRUNCATE,SW_REPAIR_SYNC,SW_FAULT } StorageWriterState;
typedef enum { STORE_TAIL_NONE,STORE_TAIL_PARTIAL,STORE_TAIL_HEADER,STORE_TAIL_CRC,
 STORE_TAIL_IDENTITY,STORE_TAIL_ORDER } StorageTailReason;
typedef struct {uint64_t session_id,generation;uint32_t slot;} StorageTicket;
typedef struct {
 StorageTicket ticket;RecordIdentity identity;uint64_t file_id,offset,total_bytes;
 StorageReceiptState state;StorageError error;
} StorageReceipt;
typedef struct {
 void *context;
 StorageIoResult (*size)(void *,uint64_t *);
 StorageIoResult (*read)(void *,uint64_t,uint8_t *,size_t,size_t *);
 StorageIoResult (*write)(void *,uint64_t,const uint8_t *,size_t,size_t *);
 StorageIoResult (*sync)(void *);
 /* Used only after an explicit repair request in SW_TAIL. */
 StorageIoResult (*truncate)(void *,uint64_t);
 /* Mandatory application schema gate. Called before sequence allocation.
  * Framing alone does not validate JSON/reference/calibration semantics. */
 bool (*validate)(void *,const RecordIdentity *,const uint8_t *,size_t);
} StorageBackend;
typedef struct {RecordIdentity identity;const uint8_t *payload;size_t payload_bytes;
 StorageTicket depends_on[STORAGE_DEPENDENCIES];unsigned dependency_count;} StorageRequest;
typedef struct {StorageResult result;bool sequence_assigned;uint64_t record_seq;StorageTicket ticket;} StorageSubmit;
typedef struct {
 bool occupied;uint64_t generation;unsigned references,dependency_count;
 StorageTicket dependencies[STORAGE_DEPENDENCIES];StorageReceipt receipt;
} StorageReceiptSlot;
typedef struct {
 StorageWriterState state;StorageError error;StorageTailReason tail_reason;
 StorageBackend backend;uint64_t session_id,file_id,next_seq,sync_interval_us,last_sync_us,last_now;
 uint64_t file_bytes,prefix,scan_last_seq,recovered_records;
 /* Rejected sequence extrema are NOT a contiguous missing range. Exact
  * record success comes from retained receipts, never a high-water guess. */
 uint64_t rejected_count,first_rejected_seq,last_rejected_seq;
 size_t used,total;unsigned active;bool have_time,force_sync;
 StorageReceiptSlot slots[STORAGE_RECEIPTS];
 uint8_t buffer[RECORD_MAX_BYTES];
} StorageWriter;
/* next_seq belongs to the exclusive session allocator. Until multi-file
 * service integration this object must be its sole owner for that session.
 * New init uses new session_id or a globally unique next_seq seed: do not
 * recreate one object mid-session with old IDs while callers retain tickets. */
bool storage_writer_init(StorageWriter *,const StorageBackend *,uint64_t session_id,
 uint64_t file_id,uint64_t next_seq,uint64_t sync_interval_us);
StorageSubmit storage_writer_submit(StorageWriter *,const StorageRequest *);
/* Sole storage owner only: append a sequence reserved by its one shared
 * admission queue. BUSY consumes no sequence; do not mix auto allocation with
 * this API in the same session. Reservations may leave explicit gaps. */
StorageSubmit storage_writer_append_reserved(StorageWriter *,const StorageRequest *,uint64_t);
StorageResult storage_writer_poll(const StorageWriter *,StorageTicket,StorageReceipt *);
StorageResult storage_writer_ack(StorageWriter *,StorageTicket);
void storage_writer_request_sync(StorageWriter *);
/* At most one <=4KiB backend read/write or one size/sync/truncate call.
 * Backend calls may block: only the dedicated storage owner may step. */
void storage_writer_step(StorageWriter *,uint64_t now_us);
/* Recovery scans the first contiguous valid same-session/strict-sequence
 * prefix. Never resynchronizes at embedded magic or promotes old receipts.
 * Recover only after backend ownership is re-established (no async IO). */
bool storage_writer_recover(StorageWriter *);
bool storage_writer_repair_tail(StorageWriter *);
#endif
