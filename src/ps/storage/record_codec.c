#include "record_codec.h"
#include "protocol.h"
#include <string.h>
static uint16_t r16(const uint8_t *b){return (uint16_t)(b[0]|((uint16_t)b[1]<<8));}
static uint32_t r32(const uint8_t *b){return b[0]|((uint32_t)b[1]<<8)|((uint32_t)b[2]<<16)|((uint32_t)b[3]<<24);}
static uint64_t r64(const uint8_t *b){return r32(b)|((uint64_t)r32(b+4)<<32);}
static void w16(uint8_t *b,uint16_t v){b[0]=(uint8_t)v;b[1]=(uint8_t)(v>>8);}
static void w32(uint8_t *b,uint32_t v){for(unsigned k=0;k<4;k++)b[k]=(uint8_t)(v>>(k*8));}
static void w64(uint8_t *b,uint64_t v){w32(b,(uint32_t)v);w32(b+4,(uint32_t)(v>>32));}
static bool valid_identity(const RecordIdentity *id){
    return id->type>=RECORD_CONFIG && id->type<=RECORD_CAPTURE && id->session_id!=0 && id->first_raw<=id->end_raw;
}
static uint32_t record_crc(const uint8_t *b,size_t payload){
    uint32_t crc=crc32c(b,20);crc=crc32c_extend(crc,b+24,40);
    return crc32c_extend(crc,b+64,payload);
}
bool record_encode(const RecordIdentity *id,const uint8_t *payload,size_t len,
                   uint8_t *dst,size_t capacity,size_t *written){
    if(written)*written=0;
    if(!written || !id || !dst || (!payload && len) || len>RECORD_MAX_BYTES-80 || capacity<80+len || !valid_identity(id))return false;
    memset(dst,0,64);w32(dst,UINT32_C(0x3152464c));w16(dst+4,1);w16(dst+6,(uint16_t)id->type);
    w16(dst+8,64);w16(dst+10,id->flags);w32(dst+12,(uint32_t)len);w32(dst+16,id->config_id);
    w64(dst+24,id->session_id);w64(dst+32,id->capture_id);w64(dst+40,id->first_raw);
    w64(dst+48,id->end_raw);w64(dst+56,id->record_seq);
    if(len)memcpy(dst+64,payload,len);
    uint32_t crc=record_crc(dst,len);w32(dst+20,crc);
    uint8_t *tail=dst+64+len;
    w32(tail,UINT32_C(0x444e454c));w32(tail+4,(uint32_t)(80+len));w32(tail+8,crc);w32(tail+12,0);
    *written=80+len;return true;
}
bool record_decode(const uint8_t *b,size_t available,RecordView *out){
    if(out)memset(out,0,sizeof(*out));
    if(!b || !out || available<80 || r32(b)!=UINT32_C(0x3152464c) || r16(b+4)!=1 || r16(b+8)!=64)return false;
    size_t payload=r32(b+12);
    if(payload>RECORD_MAX_BYTES-80 || payload+80>available)return false;
    RecordIdentity id={(RecordType)r16(b+6),r16(b+10),r32(b+16),r64(b+24),r64(b+32),r64(b+40),r64(b+48),r64(b+56)};
    if(!valid_identity(&id))return false;
    const uint8_t *tail=b+64+payload;uint32_t crc=r32(b+20);
    if(r32(tail)!=UINT32_C(0x444e454c) || r32(tail+4)!=80+payload || r32(tail+8)!=crc || r32(tail+12)!=0 || record_crc(b,payload)!=crc)return false;
    out->identity=id;out->payload=b+64;out->payload_bytes=payload;out->total_bytes=80+payload;return true;
}
size_t record_scan_prefix(const uint8_t *b,size_t len,size_t *count){
    size_t prefix=0,records=0;RecordView view;
    if(count)*count=0;
    if(!b)return 0;
    while(prefix<len && record_decode(b+prefix,len-prefix,&view)){prefix+=view.total_bytes;records++;}
    if(count)*count=records;
    return prefix;
}
