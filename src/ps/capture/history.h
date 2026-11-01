#ifndef LFN_PCM_HISTORY_H
#define LFN_PCM_HISTORY_H
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../../shared/protocol.h"
enum { HISTORY_BLOCK_SAMPLES=256,HISTORY_BLOCKS=235,HISTORY_SAMPLES=60000,HISTORY_COPY_MAX=4096 };
typedef enum { HISTORY_OK,HISTORY_BUSY,HISTORY_INVALID } HistoryStatus;
enum { HISTORY_GAP=1,HISTORY_EVICTED=2,HISTORY_CONFLICT=4,HISTORY_IDENTITY=8 };
typedef struct {uint64_t session_id,capture_id;uint32_t config_id;} HistoryIdentity;
typedef struct {
 HistoryIdentity identity;uint64_t generation,base_raw,first_raw,end_raw,lost_samples;
 bool active,have_samples;
} HistoryWatermark;
typedef struct {
 atomic_flag lock;HistoryIdentity identity;uint64_t generation,block_index,first_raw,end_raw;
 /* During wrap, offsets below split belong to block_index, remaining
  * offsets retain block_index-235. Both portions share identity/generation. */
 unsigned split;uint8_t valid[32];int32_t samples[HISTORY_BLOCK_SAMPLES];
} HistoryBlock;
typedef struct {
 atomic_flag lock;HistoryWatermark published;
 /* Capture-owner only; readers never inspect this unpublished working state. */
 HistoryWatermark owner;HistoryBlock blocks[HISTORY_BLOCKS];
} PcmHistory;
typedef struct {uint64_t first_raw,end_raw;size_t requested,valid_count;unsigned reasons;} HistoryReadResult;
/* One capture producer, concurrent bounded readers. Initialize before sharing.
 * begin invalidates old generations even with no next PCM (including RAW).
 * A failed begin must abort prepare; UINT64_MAX generation is never reused. */
void history_init(PcmHistory *);
bool history_begin(PcmHistory *,HistoryIdentity,bool monitor);
/* Private signed24 samples, optional LSB-first validity bitmap. At most256.
 * Producer tries each short lock once; never waits or keeps a source pointer.
 * BUSY records unavailable samples; later published watermark includes gaps.
 * Nonoverlapping ascending ranges on a common stride48 grid are required. */
HistoryStatus history_append(PcmHistory *,HistoryIdentity,uint64_t,const int32_t *,const uint8_t *,size_t);
HistoryStatus history_append_packet(PcmHistory *,uint64_t,const PacketView *);
/* Watermark is a frozen exclusive admitted-input end, NOT proof all samples
 * exist. It includes producer lock losses. A reader must check every valid bit.
 * Global state is copied under a short lock; no sample copies under that lock. */
bool history_watermark(PcmHistory *,HistoryWatermark *);
/* <=4096 samples; <=3 try-lock attempts and <=256 samples per block.
 * Frozen watermarks never authorize data past their end. Eviction and races
 * yield invalid bits and explicit reasons, not fabricated zero-valued PCM.
 * Caller-owned output samples at invalid bits are unspecified. False means
 * invalid request or identity changed; do not consume any samples in that case. */
bool history_copy(PcmHistory *,const HistoryWatermark *,uint64_t,size_t,int32_t *,uint8_t *,HistoryReadResult *);
#endif
