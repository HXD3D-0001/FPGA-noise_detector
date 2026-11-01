#ifndef LFN_STORAGE_QUEUE_H
#define LFN_STORAGE_QUEUE_H
#include "storage_writer.h"
enum { STORAGE_POOL_BYTES=4*1024*1024,STORAGE_BLOCK_BYTES=4096,STORAGE_QUEUE_JOBS=64,
 STORAGE_CONTROL_BLOCKS=16,STORAGE_STATUS_BLOCKS=8,STORAGE_CONTROL_JOBS=4 };
typedef struct {void *context;bool (*lock)(void *);void (*unlock)(void *);} StorageQueueLocks;
/* Optional sole-owner executor. Shared sequence is mutated ONLY under the
 * exact SAME locks in every attached queue. Never IO in admission/health.
 * Transport callbacks are consumer-only. step/append run outside queue lock;
 * poll/ack are pure no-IO/no-lock operations, callable under queue lock. */
typedef struct {uint64_t session_id,next_seq;unsigned admitted,limit;bool rolling;} StorageQueueSequence;
typedef struct {
 void *context;void (*step)(void *,uint64_t);
 StorageSubmit (*append)(void *,const StorageRequest *,uint64_t);
 StorageResult (*poll)(void *,StorageTicket,StorageReceipt *);
 StorageResult (*ack)(void *,StorageTicket);
 StorageWriterState (*status)(void *,StorageError *);
} StorageQueueTransport;
enum { STORAGE_SHARED_QUEUES=8 };
/* One4MiB arena:64KiB control +8*32KiB metadata +ordinary data.
 * Bitmap is owned by this small persistent object, all queues share its locks.
 * Initialize once, bind all queues before any producer/consumer starts. */
typedef struct {uint8_t *arena;StorageQueueLocks locks;unsigned queues;void *members[STORAGE_SHARED_QUEUES];StorageWriter *writers[STORAGE_SHARED_QUEUES];uint8_t used[STORAGE_POOL_BYTES/STORAGE_BLOCK_BYTES];} StorageQueuePool;
bool storage_queue_pool_init(StorageQueuePool *,uint8_t *,size_t,const StorageQueueLocks *);
typedef struct StorageQueueMeta StorageQueueMeta;
typedef struct {
 StorageRequest record;bool receipt_required;
} StorageQueueRequest;
typedef struct {
 uint64_t accepted,written,synced,failed,rejected,next_seq;
 unsigned queued,live_jobs,retained_receipts,used_data_blocks,used_control_blocks;
 bool ready,input_paused,input_closed;
} StorageQueueHealth;
typedef struct {
 StorageQueuePool *pool;const StorageQueueTransport *transport;StorageQueueSequence *sequence;
 StorageWriter *writer;uint8_t *arena;StorageQueueMeta *meta;StorageQueueLocks locks;
 uint64_t session_id,file_id;
 /* Immutable pure/thread-safe schema gate, never IO. */
 bool (*validate)(void *,const RecordIdentity *,const uint8_t *,size_t);void *validate_context;
 /* Private to sole consumer. Allows lock contention after an append without
  * repeating the same physical submit or freeing producer-owned memory. */
 bool pending;unsigned pending_job;StorageSubmit pending_result;
} StorageQueue;
/* Writer must be READY, without existing receipts; no more direct writer
 * submissions after attaching. Arena exactly4MiB and64-byte aligned. Control
 * payloads use64KiB, status metadata32KiB, ordinary payloads the remainder.
 * Initialize once per session; do not recreate generations while tickets live.
 * All-NULL locks only for externally serialized Host use. */
bool storage_queue_init(StorageQueue *,StorageWriter *,uint8_t *,size_t,const StorageQueueLocks *);
/* Shared queue starts NOT READY; submit/step blocked until transport bound. */
bool storage_queue_init_shared(StorageQueue *,StorageWriter *,StorageQueuePool *);
/* Sealed EVENT replacement only: same pool/transport/sequence, no receipts or
 * jobs. Keeps receipt generations; starts paused until durable catalog OPEN. */
StorageResult storage_queue_rebind_empty(StorageQueue *,StorageWriter *);
/* Bind BEFORE first submit/service_init, external exclusive owner setup only.
 * At most sequence.limit accepted jobs in this allocation epoch; rejection
 * holes still consume global sequence as in the original queue contract. */
bool storage_queue_bind_transport(StorageQueue *,const StorageQueueTransport *,StorageQueueSequence *);
/* Sole owner lifecycle fencing. Pause ALL pristine queues atomically before
 * catalog IO. Resume after durable OPEN. Close only an empty drained queue;
 * permanent for this queue lifetime, receipts remain pollable/ACKable. No IO. */
StorageResult storage_queue_pause_pristine(StorageQueue **,unsigned);
StorageResult storage_queue_resume_pristine(StorageQueue *);
StorageResult storage_queue_close_empty(StorageQueue *,StorageQueueHealth *);
StorageResult storage_queue_head_sequence(StorageQueue *,uint64_t *);
/* Multi-producer task APIs: bounded try-lock, value copies, no backend IO.
 * Capacity refusals consume record_seq; lock BUSY/invalid do not. */
StorageSubmit storage_queue_submit(StorageQueue *,const StorageQueueRequest *);
/* Bounded definition closure may reserve admission slots while a data backlog
 * is saturated; normal producers/consumers use storage_queue_submit only. */
StorageSubmit storage_queue_submit_reserved(StorageQueue *,const StorageQueueRequest *,bool reserve);
StorageResult storage_queue_poll(StorageQueue *,StorageTicket,StorageReceipt *);
StorageResult storage_queue_ack(StorageQueue *,StorageTicket);
StorageResult storage_queue_health(StorageQueue *,StorageQueueHealth *);
/* Sole storage executor, not ISR/capture. IO always outside queue lock.
 * At most one E1 IO step, then one attempted queued dispatch per call. */
void storage_queue_step(StorageQueue *,uint64_t now_us);
/* Sole executor: all queued jobs first resolved. Recovery never promotes old
 * failed public receipts; re-establish file ownership before calling. */
bool storage_queue_recover(StorageQueue *);
bool storage_queue_repair_tail(StorageQueue *);
#endif
