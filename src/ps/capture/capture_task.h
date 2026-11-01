#ifndef LFN_CAPTURE_TASK_H
#define LFN_CAPTURE_TASK_H
#include "../drivers/dma_rx.h"
#include "../app/block_pool.h"
#include "../drivers/pl_regs.h"
enum { CAPTURE_POLL_US=20000, CAPTURE_ROUND_US=2000, CAPTURE_PACKETS_PER_FLOW=4, CAPTURE_ENERGY_PER_ROUND=32 };
typedef enum { CAPTURE_DMA_ERROR, CAPTURE_LENGTH, CAPTURE_CRC, CAPTURE_PROTOCOL,
    CAPTURE_IDENTITY, CAPTURE_FLAGS, CAPTURE_RANGE, CAPTURE_REORDER,
    CAPTURE_POOL_FULL, CAPTURE_POOL_BUSY, CAPTURE_REARM_FAILED, CAPTURE_CONTRACT, CAPTURE_REASON_COUNT } CaptureReject;
typedef struct {
    uint64_t session_id, capture_id, origin_raw;
    uint32_t config_id;
    bool raw_mode;
    /* Literal first candidate sequence per FFT/PCM/RAW. Counts do not reset on
     * DMA recovery; mode/config changes require an entirely new identity. */
    uint64_t first_seq[4]; /* FFT, PCM, RAW, ENERGY */
} CaptureConfig;

/* Opt-in contract for actual ADC/TEST grid metadata. Never derive these fields
 * from active_capture_id+1, BOOT_ORIGIN or the first successfully received packet.
 * Hardware reservation/counter metadata is required before preparing DMA. */
enum { CAPTURE_STREAM_ENERGY=4, CAPTURE_MONITOR_MASK=0x0b, CAPTURE_RAW_MASK=0x04 };
typedef struct {
    uint64_t session_id,capture_id;
    uint32_t config_id;
    bool raw_mode;
    uint64_t first_seq[4]; /* FFT, PCM, RAW, ENERGY: literal candidate counters */
} CapturePlan;
typedef struct {
    uint64_t session_id,capture_id;
    uint32_t config_id;
    unsigned stream; /* LFN_FFT / LFN_PCM / LFN_RAW / CAPTURE_STREAM_ENERGY */
    uint64_t first_seq,first_raw;
} CaptureAnchor;
typedef struct {
    void *context;
    uint64_t (*monotonic_us)(void *);
    void (*notify_task)(void *);
    /* All consumers synchronous, may not retain DMA memory. No blocking IO. */
    void (*on_packet)(void *,const PacketView *);
    void (*on_reject)(void *,CaptureReject,const PacketHeader *);
    void (*on_gap)(void *,uint16_t,uint64_t,uint64_t);
    /* Adapter performs one real bounded LATCH/read/POP transaction. */
    bool (*energy_read)(void *,PlEnergy *);
    /* Nonblocking bounded lock shared with analysis_service. Both NULL only
     * for externally serialized single-thread use. Lock covers at most one
     * 32832B copy; use mutex (no IRQ masking) and measure ARM critical duration. */
    bool (*pool_lock)(void *);
    void (*pool_unlock)(void *);
} CaptureHooks;
enum { CAPTURE_ENERGY_SLOTS=128, ENERGY_GAP_SOURCE=1, ENERGY_GAP_POOL=2 };
typedef struct {
    bool gap;
    PlEnergy record;
    uint64_t first_missing_seq,end_missing_seq;
    uint32_t reason;
} CaptureEnergyMessage;
typedef struct {
    DmaRx *flows[2]; /* [0] FFT/RAW, [1] PCM */
    BlockPool *pool;
    CaptureConfig config;
    CaptureHooks hooks;
    uint64_t next_seq[3], rejects[CAPTURE_REASON_COUNT], accepted[3], missing[3];
    uint64_t last_round_us, max_service_interval_us, max_packet_us, max_round_us;
    unsigned next_flow;
    bool has_round;
    CaptureEnergyMessage energy_queue[CAPTURE_ENERGY_SLOTS],energy_pending;
    unsigned energy_head,energy_count,energy_highwater;
    uint64_t energy_next_seq,energy_dropped,energy_missing,energy_rejected;
    bool energy_pending_valid;
    bool explicit_contract;
    uint8_t anchor_bound_mask;
    uint64_t first_raw[4]; /* immutable per-stream grid origins, not latest raw */
} CaptureTask;
bool capture_task_init(CaptureTask *,DmaRx *spectral,DmaRx *pcm,BlockPool *,const CaptureConfig *,const CaptureHooks *);
bool capture_task_set_config(CaptureTask *,const CaptureConfig *); /* only quiesced FREE rings */
/* Same sole task owns prepare/bind/run/submit/reset: not callable from ISR.
 * prepare needs FREE rings/pool/energy queues and a new capture identity.
 * bind is metadata-only: safe with DMA-owned buffers, never frees/rearms them.
 * Rebinding is allowed only for the identical anchor. Pending streams are not
 * polled/POPed by run; bound streams continue independently. */
bool capture_task_init_plan(CaptureTask *,DmaRx *,DmaRx *,BlockPool *,const CapturePlan *,const CaptureHooks *);
bool capture_task_prepare(CaptureTask *,const CapturePlan *);
/* Pure preflight supports atomic multi-stream bind in the same owner task. */
bool capture_task_check_anchor(const CaptureTask *,const CaptureAnchor *);
bool capture_task_bind_anchor(CaptureTask *,const CaptureAnchor *);
uint8_t capture_task_pending_anchors(const CaptureTask *);
/* After source/PL drain, account for every complete grid packet through the
 * last accepted raw sample. Existing loss records remain losses; this grants
 * no quality PASS. Partial final windows are not packets. */
bool capture_task_received_through(const CaptureTask *,uint64_t last_raw);
size_t capture_task_run(CaptureTask *); /* task context, bounded nonblocking pass */
bool capture_task_poll_due(const CaptureTask *,uint64_t now_us);
/* Pure synchronous validation/copy entry; callers own source lifetime. */
bool capture_submit(CaptureTask *,unsigned flow,const uint8_t *,size_t actual_length,bool dma_packet_ok);
bool capture_energy_submit(CaptureTask *,const PlEnergy *);
/* Serialized with submit via bounded platform lock. Draining returns the
 * retained final GapNotice even with no later hardware record (STOP barrier). */
bool capture_energy_take(CaptureTask *,CaptureEnergyMessage *);
#endif
