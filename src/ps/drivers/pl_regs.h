#ifndef LFN_PL_REGS_H
#define LFN_PL_REGS_H
#include <stdint.h>
/* One control service owns commands/commit/snapshot. capture_task is the sole
 * energy consumer. Callbacks are synchronous MMIO with device memory ordering;
 * write32 returns nonzero on a bus fault. Base address comes from actual BSP. */
typedef struct {
 void *context;
 uint32_t (*read32)(void *, uint32_t offset);
 int (*write32)(void *, uint32_t offset, uint32_t value);
 void (*poll_wait)(void *);
 uint32_t max_polls;
} PlRegs;
typedef enum { PL_OK=0, PL_INVALID=-1, PL_BUS=-2, PL_REJECTED=-3,
 PL_TIMEOUT=-4, PL_STALE=-5, PL_EMPTY=-6, PL_BUSY=-7 } PlResult;
enum { PL_START=1, PL_STOP=2, PL_RESET=4 };
enum { PL_BUSY_BIT=1, PL_DONE_BIT=2, PL_REJECTED_BIT=4,
 PL_TIMEOUT_BIT=8, PL_VALID_BIT=16, PL_STALE_BIT=32 };
enum { PL_MODE_MONITOR=0,PL_MODE_RAW_CAL=1,PL_MODE_RAW_BASELINE=2,PL_MODE_TEST_PATTERN=3 };
enum { PL_REASON_BUSY=1,PL_REASON_STATE=2,PL_REASON_MALFORMED=3,
 PL_REASON_CONFIG=4,PL_REASON_SERVICE=5,PL_REASON_TIMEOUT=6,PL_REASON_EXHAUSTED=7 };
typedef struct {
 uint64_t seq,capture_id,first_raw,energy;
 uint32_t config_id,flags;
} PlEnergy;
typedef struct {
 uint32_t seq,source_id,source_position_valid;
 uint64_t counters[16],latest_raw,origin_raw,age_ticks,energy_flush_count,pcm_flush_count;
 uint64_t fft_first_seq,pcm_first_seq,energy_first_seq,raw_first_seq;
} PlSnapshot;
typedef struct {uint32_t seq,mask;int reset;} PlDmaAction;
PlResult pl_command(const PlRegs *,uint32_t command);
PlResult pl_commit(const PlRegs *,uint32_t mode,uint32_t config_id);
PlResult pl_snapshot(const PlRegs *,uint64_t max_age_ticks,PlSnapshot *);
PlResult pl_energy_read(const PlRegs *,PlEnergy *);
/* reset==1 requires hardware reset completion; reset==0 requires observing both
 * requested DMA streams idle after the PL drain barrier. Never ACK a request
 * merely because a reset/drain was submitted. */
PlResult pl_dma_action_read(const PlRegs *,PlDmaAction *);
PlResult pl_dma_action_ack(const PlRegs *,uint32_t seq,uint32_t mask);
/* Asynchronous: caller must service the tagged DMA action while BUSY. A late
 * genuine ACK may finish an expired operation; STATUS preserves timeout bit. */
PlResult pl_flow_recover_request(const PlRegs *,uint32_t mask,uint32_t *seq);
PlResult pl_flow_recover_poll(const PlRegs *,uint32_t seq);
#endif
