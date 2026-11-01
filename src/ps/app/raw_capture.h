#ifndef LFN_RAW_CAPTURE_H
#define LFN_RAW_CAPTURE_H
#include "../../shared/protocol.h"
/* Fixed complete protocol-v1 RAW packets, header + 8192 signed24-in-int32 values.
 * Capacity is wire bytes, not persisted bytes or actual elapsed sample time. */
enum { LFN_RAW_PACKET_BYTES=32832, LFN_RAW_BUFFER_BYTES=8*1024*1024 };
typedef enum { RAW_STOP_NONE,RAW_STOP_TIME,RAW_STOP_CAPACITY,RAW_STOP_MANUAL,
 RAW_STOP_INPUT_LOSS,RAW_STOP_CLOCK,RAW_STOP_LOCK_BUSY,RAW_STOP_FAULT } LfnRawStop;
typedef enum { RAW_OK,RAW_EMPTY,RAW_BUSY,RAW_STALE,RAW_INVALID } LfnRawResult;
typedef struct {uint64_t duration_us,capacity_bytes;} LfnRawPolicy;
typedef struct {
 uint64_t session_id,capture_id;uint32_t config_id;
 LfnRawPolicy policy;uint64_t started_us,stored_bytes,packets,ignored_after_stop;
 LfnRawStop reason;bool prepared,started,terminal,cleanup_failed;
} LfnRawStatus;
typedef struct {void *context;bool (*lock)(void *);void (*unlock)(void *);} LfnRawLocks;
typedef struct {
 uint8_t *buffer;size_t buffer_bytes;LfnRawLocks locks;
 /* Sole producer owns status. Readers touch only the guarded committed index.
  * Status reaches other tasks through the service snapshot, never directly. */
 LfnRawStatus status;uint64_t last_us,read_session,read_capture;uint32_t read_config;size_t committed;
} LfnRawCapture;
bool lfn_raw_init(LfnRawCapture *,uint8_t *,size_t,const LfnRawLocks *);
bool lfn_raw_policy_valid(const LfnRawCapture *,const LfnRawPolicy *);
/* Begin only with all old capture owners quiescent. Replaces previous RAM
 * capture; a busy lock leaves it intact. Caller must authorize the new capture. */
bool lfn_raw_begin(LfnRawCapture *,uint64_t,uint64_t,uint32_t,const LfnRawPolicy *);
void lfn_raw_start(LfnRawCapture *,uint64_t now_us);
void lfn_raw_tick(LfnRawCapture *,uint64_t now_us);
void lfn_raw_stop(LfnRawCapture *,LfnRawStop);
void lfn_raw_finish(LfnRawCapture *,bool failed);
/* Synchronous copied bytes. No borrowed DMA pointers retained; no disk IO. */
LfnRawResult lfn_raw_offer(LfnRawCapture *,const uint8_t *,size_t,uint64_t now_us);
/* Read a complete packet by ordinal with exact identity. Output remains
 * untouched on error, including stale identity after a new begin. */
LfnRawResult lfn_raw_read(LfnRawCapture *,uint64_t,uint64_t,uint32_t,uint64_t,
 uint8_t *,size_t);
#endif
