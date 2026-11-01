#ifndef LFN_BLOCK_POOL_H
#define LFN_BLOCK_POOL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../../shared/protocol.h"
enum { LFN_BLOCK_SLOTS=8 };
typedef struct { uint16_t slot; uint64_t generation; } BlockHandle;
typedef enum { BLOCK_FREE, BLOCK_PRODUCER, BLOCK_QUEUED, BLOCK_CONSUMER } BlockState;
typedef struct { _Alignas(64) uint8_t bytes[LFN_MAX_PACKET]; } PacketBlock;
typedef struct {
    PacketBlock blocks[LFN_BLOCK_SLOTS];
    BlockState state[LFN_BLOCK_SLOTS];
    uint64_t generation[LFN_BLOCK_SLOTS];
    size_t length[LFN_BLOCK_SLOTS];
    BlockHandle queue[LFN_BLOCK_SLOTS];
    unsigned head, count, highwater;
} BlockPool;
/* Serialize producer/consumer calls with a bounded priority-inheriting lock or
 * equivalent platform critical section. No function allocates or waits. */
void block_pool_init(BlockPool *pool);
bool block_pool_acquire(BlockPool *pool, BlockHandle *handle, uint8_t **bytes);
bool block_pool_publish(BlockPool *pool, BlockHandle handle, size_t length);
bool block_pool_take(BlockPool *pool, BlockHandle *handle, const uint8_t **bytes, size_t *length);
bool block_pool_release(BlockPool *pool, BlockHandle handle);
unsigned block_pool_free_count(const BlockPool *pool);
#endif
