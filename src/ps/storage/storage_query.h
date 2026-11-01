#ifndef LFN_STORAGE_QUERY_H
#define LFN_STORAGE_QUERY_H
#include "storage_history.h"
#include "storage_queue.h"
enum { STORAGE_QUERY_SLOTS=2,STORAGE_QUERY_CHUNK=4096 };
typedef enum { QUERY_LIVE_SYNC_PREFIX,QUERY_HISTORICAL_CLAIM_UNVERIFIED } StorageQueryProvenance;
typedef struct {uint64_t session_id,file_id,first_sequence,end_sequence,capture_id;uint32_t config_id,type_mask;bool include_definitions;} StorageQueryRequest;
typedef struct {uint64_t generation;uint32_t slot;} StorageQueryTicket;
typedef enum { QUERY_COMPLETE,QUERY_CANCELLED,QUERY_NOT_FOUND,QUERY_DATA_EXPIRED,QUERY_IO,QUERY_CORRUPT,QUERY_METADATA_CAPACITY,QUERY_NOT_READY,QUERY_DATA_MISSING } StorageQueryStatus;
typedef struct {StorageQueryStatus status;StorageQueryProvenance provenance;uint64_t accepted_prefix,snapshot_bytes,scanned_records,exported_records;} StorageQueryResult;
typedef struct {uint64_t generation,file_offset;size_t record_offset,bytes;RecordIdentity identity;bool first,last;StorageQueryProvenance provenance;uint8_t data[STORAGE_QUERY_CHUNK];} StorageQueryChunk;
typedef struct {StorageBackend backend;uint64_t session_id,file_id,bytes,pin;bool exact_size,check_crc;uint32_t expected_crc;StorageQueryProvenance provenance;} StorageQuerySource;
typedef enum { QUERY_SOURCE_OK,QUERY_SOURCE_BUSY,QUERY_SOURCE_MISSING,QUERY_SOURCE_EXPIRED,QUERY_SOURCE_IO,QUERY_SOURCE_NOT_READY,QUERY_SOURCE_DATA_MISSING,QUERY_SOURCE_CORRUPT } StorageQueryOpen;
typedef struct {
 void *context;StorageQueryOpen (*open)(void *,const StorageQueryRequest *,StorageQuerySource *);
 StorageIoResult (*release)(void *,StorageQuerySource *);
} StorageQueryFiles;
typedef struct {
 uint64_t generation,order;unsigned state;bool cancel;StorageQueryRequest request;
 StorageQueryChunk chunk;StorageQueryResult result;
} StorageQuerySlot;
typedef struct {
 StorageQueueLocks locks;StorageQueryFiles files;StorageHistoryReader *reader;
 StorageQuerySlot slots[STORAGE_QUERY_SLOTS];uint64_t next_order;unsigned active,phase;
 StorageQuerySource source;StorageQueryResult completion;size_t exported;bool have_source;
} StorageQuery;
/* One storage consumer, two bounded client slots. reader persistent~600KiB.
 * File callbacks ONLY in step, outside mailbox lock, <=one owner IO each.
 * open atomically pins verified inventory key or reports EXPIRED by retention;
 * historical sources are never upgraded to live persistence proof. release
 * failure must keep source protected in provider until explicit recovery. */
bool storage_query_init(StorageQuery *,StorageHistoryReader *,const StorageQueueLocks *,const StorageQueryFiles *);
StorageResult storage_query_submit(StorageQuery *,const StorageQueryRequest *,StorageQueryTicket *);
StorageResult storage_query_copy(StorageQuery *,StorageQueryTicket,StorageQueryChunk *);
StorageResult storage_query_ack_chunk(StorageQuery *,StorageQueryTicket,uint64_t chunk_generation);
StorageResult storage_query_result(StorageQuery *,StorageQueryTicket,StorageQueryResult *);
StorageResult storage_query_ack_result(StorageQuery *,StorageQueryTicket);
StorageResult storage_query_cancel(StorageQuery *,StorageQueryTicket);
/* Caller services high-water writes, due sync and FAULT before query work.
 * One copied <=4KiB LFR1 chunk, one reader IO, or one source callback per step.
 * Slow clients pin their chunk/file, NEVER hold DMA data or wait in storage.
 * FIFO admission and retained terminal response; do not reinit live tickets. */
void storage_query_step(StorageQuery *);
#endif
