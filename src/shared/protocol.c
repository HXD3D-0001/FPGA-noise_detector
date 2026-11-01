#include "protocol.h"
#include <string.h>

static uint16_t get16(const uint8_t *b) { return (uint16_t)(b[0]|((uint16_t)b[1]<<8)); }
static uint32_t get32(const uint8_t *b) {
    return (uint32_t)b[0]|((uint32_t)b[1]<<8)|((uint32_t)b[2]<<16)|((uint32_t)b[3]<<24);
}
static uint64_t get64(const uint8_t *b) { return get32(b)|((uint64_t)get32(b+4)<<32); }
static void put16(uint8_t *b,uint16_t v) { b[0]=(uint8_t)v; b[1]=(uint8_t)(v>>8); }
static void put32(uint8_t *b,uint32_t v) { for(unsigned i=0;i<4;i++)b[i]=(uint8_t)(v>>(8*i)); }
static void put64(uint8_t *b,uint64_t v) { put32(b,(uint32_t)v);put32(b+4,(uint32_t)(v>>32)); }
static int32_t signed_value(const uint8_t *b) {
    uint32_t u=get32(b);
    return (int32_t)((u & UINT32_C(0x80000000)) ? (int64_t)u-INT64_C(4294967296) : (int64_t)u);
}
static bool signed24(int32_t n) { return n>=-8388608 && n<=8388607; }
static uint32_t crc_update(uint32_t state,const uint8_t *b,size_t n) {
    for(size_t i=0;i<n;i++) {
        state^=b[i];
        for(unsigned j=0;j<8;j++) state=(state>>1)^((0u-(state&1u))&UINT32_C(0x82f63b78));
    }
    return state;
}
uint32_t crc32c(const uint8_t *b,size_t n) { return crc_update(UINT32_MAX,b,n)^UINT32_MAX; }
uint32_t crc32c_extend(uint32_t checksum,const uint8_t *b,size_t n) {
    return crc_update(checksum^UINT32_MAX,b,n)^UINT32_MAX;
}
static uint32_t packet_crc(const uint8_t *b,size_t n) {
    return crc_update(crc_update(UINT32_MAX,b,60),b+64,n-64)^UINT32_MAX;
}
static bool shape(uint16_t type,size_t *count,uint32_t *samples,uint32_t *stride,uint8_t *format) {
    switch(type) {
    case LFN_FFT:*count=8192;*samples=4096;*stride=48;*format=2;return true;
    case LFN_PCM:*count=256;*samples=256;*stride=48;*format=1;return true;
    case LFN_RAW:*count=8192;*samples=8192;*stride=1;*format=1;return true;
    default:return false; /* STATUS is reserved, not a normal DMA packet. */
    }
}
static bool valid_header(const PacketHeader *h,uint32_t samples,uint32_t stride) {
    return h->capture_id!=0 && h->config_id!=0 && !(h->flags & ~UINT16_C(63)) &&
           (h->type==LFN_FFT ? h->blk_exp<=12 : h->blk_exp==0) &&
           h->first_raw_sample_index<=UINT64_MAX-(uint64_t)samples*stride;
}
bool packet_encode(const PacketHeader *h,const int32_t *values,size_t count,
                   uint8_t *dst,size_t capacity,size_t *written) {
    size_t required; uint32_t samples,stride; uint8_t format;
    if(written) *written=0;
    if(!written || !h || !values || !dst || !shape(h->type,&required,&samples,&stride,&format)) return false;
    const size_t total=64+required*4;
    if(count!=required || capacity<total || !valid_header(h,samples,stride)) return false;
    for(size_t i=0;i<count;i++) if(!signed24(values[i])) return false;
    memset(dst,0,64);
    put32(dst,UINT32_C(0x314e464c));put16(dst+4,1);put16(dst+6,h->type);
    put16(dst+8,64);put16(dst+10,h->flags);put32(dst+12,(uint32_t)(count*4));
    put64(dst+16,h->capture_id);put64(dst+24,h->stream_seq);put64(dst+32,h->first_raw_sample_index);
    put32(dst+40,samples);put32(dst+44,stride);put32(dst+48,h->config_id);put32(dst+52,48000);
    put16(dst+56,24);dst[58]=h->blk_exp;dst[59]=format;
    for(size_t i=0;i<count;i++) put32(dst+64+4*i,(uint32_t)values[i]);
    put32(dst+60,packet_crc(dst,total));*written=total;return true;
}
bool packet_decode(const uint8_t *b,size_t len,PacketView *out) {
    size_t count;uint32_t samples,stride;uint8_t format;
    if(out) memset(out,0,sizeof(*out));
    if(!b || !out || len<64 || get32(b)!=UINT32_C(0x314e464c) || get16(b+4)!=1 || get16(b+8)!=64) return false;
    if(!shape(get16(b+6),&count,&samples,&stride,&format) || len!=64+count*4 ||
       get32(b+12)!=count*4 || get32(b+40)!=samples || get32(b+44)!=stride ||
       get32(b+52)!=48000 || get16(b+56)!=24 || b[59]!=format) return false;
    PacketHeader h={get16(b+6),get16(b+10),get64(b+16),get64(b+24),get64(b+32),get32(b+48),b[58]};
    if(!valid_header(&h,samples,stride) || get32(b+60)!=packet_crc(b,len)) return false;
    for(size_t i=0;i<count;i++) if(!signed24(signed_value(b+64+4*i))) return false;
    out->header=h;out->payload=b+64;out->payload_bytes=count*4;out->value_count=count;
    out->source_sample_count=samples;out->raw_ticks_per_sample=stride;
    out->end_raw=h.first_raw_sample_index+(uint64_t)samples*stride;
    return true;
}
bool packet_value(const PacketView *p,size_t i,int32_t *out) {
    if(!p || !p->payload || !out || i>=p->value_count) return false;
    *out=signed_value(p->payload+i*4);return true;
}
