#ifndef LFN_RECORD_CODEC_H
#define LFN_RECORD_CODEC_H
#include "record_schema.h"
#include <stdbool.h>
#include <stddef.h>
enum { RECORD_HEADER_BYTES=64, RECORD_TAIL_BYTES=16, RECORD_MAX_BYTES=65536 };
typedef struct {
    RecordIdentity identity;
    const uint8_t *payload;
    size_t payload_bytes,total_bytes;
} RecordView;
bool record_encode(const RecordIdentity *id,const uint8_t *payload,size_t len,
                   uint8_t *dst,size_t capacity,size_t *written);
bool record_decode(const uint8_t *bytes,size_t available,RecordView *out);
/* Returns the complete prefix. A corrupt/truncated record stops scanning;
 * embedded magic bytes are never treated as permission to skip corruption. */
size_t record_scan_prefix(const uint8_t *bytes,size_t len,size_t *record_count);
#endif
