#ifndef LFN_PROTOCOL_H
#define LFN_PROTOCOL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { LFN_HEADER_BYTES=64, LFN_SAMPLE_BITS=24, LFN_FFT_COUNT=4096,
       LFN_PCM_COUNT=256, LFN_RAW_COUNT=8192, LFN_MAX_PACKET=32832 };
typedef enum { LFN_FFT=1, LFN_PCM=2, LFN_RAW=3 } PacketType;
enum { LFN_VALID=1, LFN_WARMUP=2, LFN_GAP=4, LFN_SATURATION=8,
       LFN_PROTOCOL_ERROR=16, LFN_CLOCK_ERROR=32 };
typedef struct {
    uint16_t type, flags;
    uint64_t capture_id, stream_seq, first_raw_sample_index;
    uint32_t config_id;
    uint8_t blk_exp;
} PacketHeader;
typedef struct {
    PacketHeader header;
    const uint8_t *payload; /* Borrowed only for the lifetime of input buffer. */
    size_t payload_bytes, value_count;
    uint32_t source_sample_count, raw_ticks_per_sample;
    uint64_t end_raw;
} PacketView;
uint32_t crc32c(const uint8_t *buf, size_t len);
uint32_t crc32c_extend(uint32_t checksum,const uint8_t *buf,size_t len);
bool packet_encode(const PacketHeader *header, const int32_t *values,
                   size_t count, uint8_t *dst, size_t capacity, size_t *written);
bool packet_decode(const uint8_t *buf, size_t len, PacketView *out);
bool packet_value(const PacketView *packet, size_t index, int32_t *out);
#endif
