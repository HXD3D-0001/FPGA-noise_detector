#ifndef LFN_DMA_RX_H
#define LFN_DMA_RX_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
enum { DMA_RX_MAX_SLOTS=128, DMA_RX_FFT_SLOTS=32, DMA_RX_PCM_SLOTS=128,
       DMA_RX_FFT_CAPACITY=65536, DMA_RX_PCM_CAPACITY=2048 };
typedef enum { DMA_FREE, DMA_PREPARED, DMA_OWNED, DMA_CPU_OWNED, DMA_QUARANTINED } DmaOwnership;
typedef struct { uint16_t slot; uint64_t epoch, generation; } DmaToken;
typedef struct { DmaToken token; size_t actual_length; bool complete_packet, error; } DmaCompletion;
typedef struct {
    void *context;
    void (*clean)(void *,void *,size_t);
    void (*invalidate)(void *,void *,size_t);
    void (*barrier)(void *);
    bool (*submit)(void *,DmaToken,void *,size_t);
    bool (*poll)(void *,DmaCompletion *);
    bool (*reset_start)(void *);
    bool (*reset_done)(void *);
    uint32_t (*irq_ack)(void *);
    void (*notify_from_isr)(void *,uint32_t);
    /* Optional SG post-work→FREE descriptor operation, only after synchronous
     * CPU consumption. Failure preserves CPU ownership and requires reset. */
    bool (*reclaim)(void *,DmaToken);
} DmaBackend;
typedef struct { DmaOwnership state; uint64_t generation; size_t length; bool packet_ok; } DmaSlot;
typedef struct DmaRx {
    DmaBackend backend;
    uint8_t *memory;
    size_t capacity, count;
    DmaSlot slots[DMA_RX_MAX_SLOTS];
    uint64_t epoch, stale_completions, bad_completions, submission_failures;
    uint32_t irq_status;
    bool resetting, fault, submissions_paused;
    void *consumer_context;
    void (*consume)(void *,struct DmaRx *,DmaToken,const uint8_t *,size_t,bool);
} DmaRx;
/* Hardware BD memory belongs to backend, which must use actual driver BD
 * alignment/stride, sync descriptor cache lines and confirm SG completion.
 * All hooks required (cache-disabled hooks may be explicit no-ops). API is
 * task-serialized. irq_ack/notify_from_isr alone may run in ISR. */
bool dma_rx_init(DmaRx *,const DmaBackend *,void *memory,size_t capacity,size_t slots);
bool dma_rx_prepare(DmaRx *,size_t slot,DmaToken *);
void dma_rx_pause_submissions(DmaRx *,bool paused); /* task-only, ownership retained */
bool dma_rx_submit(DmaRx *,DmaToken);
bool dma_rx_complete(DmaRx *,DmaCompletion,DmaToken *,const uint8_t **,size_t *,bool *);
bool dma_rx_release(DmaRx *,DmaToken);
/* End a failed synchronous CPU borrow without reusing its memory. Recovery
 * may now reset/quarantine the stream; outstanding external borrows forbidden. */
bool dma_rx_quarantine_cpu(DmaRx *,DmaToken);
size_t dma_rx_drain(DmaRx *,size_t max_packets);
bool dma_rx_reset_begin(DmaRx *);
bool dma_rx_reset_poll(DmaRx *);
void dma_rx_isr(DmaRx *);
#endif
