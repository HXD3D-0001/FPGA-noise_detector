#include "ps_smoke.h"
#include "protocol.h"

/* Static storage keeps the 4 KiB ARM stack independent of packet size. */
static int32_t sample_values[LFN_PCM_COUNT];
static uint8_t packet_bytes[LFN_HEADER_BYTES+LFN_PCM_COUNT*4];

uint32_t ps_smoke_response(uint32_t nonce) {
    uint32_t x=nonce^UINT32_C(0xa5c39e71);
    return ((x<<7)|(x>>25))^UINT32_C(0x6d2b79f5);
}

uint32_t ps_smoke_self_test(void) {
    uint32_t checks=0;
    if(crc32c((const uint8_t *)"123456789",9)==UINT32_C(0xe3069283))
        checks|=PS_SMOKE_CRC;
    for(unsigned i=0;i<LFN_PCM_COUNT;i++) {
        switch(i&7u) {
        case 0:sample_values[i]=-8388608;break;
        case 1:sample_values[i]=8388607;break;
        case 2:sample_values[i]=-1;break;
        case 3:sample_values[i]=0;break;
        default: {
            int32_t value=(int32_t)((i*UINT32_C(65537)+12345u)&UINT32_C(0x7fffff));
            sample_values[i]=(i&1u) ? -value : value;
            break;
        }
        }
    }
    const PacketHeader expected={LFN_PCM,LFN_VALID|LFN_GAP|LFN_SATURATION,
        UINT64_C(0xfedcba9876543210),UINT64_C(0x1234567887654321),
        UINT64_C(0x0123456789abc000),UINT32_C(0x89abcdef),0};
    size_t written=0;
    PacketView view;
    if(!packet_encode(&expected,sample_values,LFN_PCM_COUNT,packet_bytes,sizeof(packet_bytes),&written)
       || written!=sizeof(packet_bytes)
       || !packet_decode(packet_bytes,written,&view)) return checks;
    if(view.header.type==expected.type && view.header.flags==expected.flags
       && view.header.capture_id==expected.capture_id
       && view.header.stream_seq==expected.stream_seq
       && view.header.first_raw_sample_index==expected.first_raw_sample_index
       && view.header.config_id==expected.config_id && view.header.blk_exp==0
       && view.payload_bytes==LFN_PCM_COUNT*4 && view.value_count==LFN_PCM_COUNT
       && view.source_sample_count==LFN_PCM_COUNT && view.raw_ticks_per_sample==48
       && view.end_raw==expected.first_raw_sample_index+UINT64_C(256)*48)
        checks|=PS_SMOKE_METADATA;
    bool values_ok=true;
    for(unsigned i=0;i<LFN_PCM_COUNT;i++) {
        int32_t decoded=0;
        bool ok=packet_value(&view,i,&decoded);
        if(!ok || decoded!=sample_values[i]) values_ok=false;
    }
    int32_t ignored=0;
    if(packet_value(&view,LFN_PCM_COUNT,&ignored)) values_ok=false;
    if(values_ok) checks|=PS_SMOKE_VALUES;
    packet_bytes[LFN_HEADER_BYTES+9]^=1u;
    if(!packet_decode(packet_bytes,written,&view)) checks|=PS_SMOKE_REJECT_CRC;
    return checks;
}
