#include "audio_regs.h"
static int valid(const PlRegs*r){return r&&r->read32&&r->write32&&r->max_polls;}
static uint32_t rd(const PlRegs*r,uint32_t a){return r->read32(r->context,a);}
static uint64_t pair(const PlRegs*r,uint32_t a){uint64_t v=rd(r,a);return v|((uint64_t)rd(r,a+4)<<32);}
PlResult pl_audio_probe(const PlRegs*r){
 if(!valid(r))return PL_INVALID;
 return rd(r,0)==0x31434c41&&rd(r,4)==0x10000?PL_OK:PL_REJECTED;
}
static PlResult wait_done(const PlRegs*r,uint32_t a){
 uint32_t i;
 for(i=0;i<r->max_polls;i++){
  uint32_t s=rd(r,a);
  if(s&PL_TIMEOUT_BIT)return PL_TIMEOUT;
  if(!(s&PL_BUSY_BIT))return s&PL_REJECTED_BIT?PL_REJECTED:(s&PL_DONE_BIT?PL_OK:PL_STALE);
  if(r->poll_wait)r->poll_wait(r->context);
 }
 return PL_TIMEOUT;
}
PlResult pl_audio_command(const PlRegs*r,uint32_t kind){
 PlResult result;uint32_t before;
 if(kind!=1&&kind!=2)return PL_INVALID;
 result=pl_audio_probe(r);if(result!=PL_OK)return result;
 if((rd(r,0xc)&1)||rd(r,0x14))return PL_BUSY;
 before=rd(r,0x10);if(before==UINT32_MAX)return PL_REJECTED;
 if(r->write32(r->context,8,kind))return PL_BUS;
 result=wait_done(r,0xc);if(result!=PL_OK)return result;
 return rd(r,0x10)==before+1?PL_OK:PL_STALE;
}
PlResult pl_audio_snapshot(const PlRegs*r,uint64_t max_age,PlAudioSnapshot*out){
 PlAudioSnapshot v;PlResult result;uint32_t before,s;
 if(!out)return PL_INVALID;
 result=pl_audio_probe(r);if(result!=PL_OK)return result;
 if(rd(r,0x24)&1)return PL_BUSY;
 before=rd(r,0x28);
 if(r->write32(r->context,0x20,1))return PL_BUS;
 result=wait_done(r,0x24);if(result!=PL_OK)return result;
 v.seq=rd(r,0x28);v.flags=rd(r,0x40);v.schema=rd(r,0x44);
 v.capture_id=pair(r,0x48);v.adc_count=pair(r,0x50);v.test_count=pair(r,0x58);
 v.latest_raw=pair(r,0x60);v.fault_count=pair(r,0x68);v.config_id=rd(r,0x70);
 v.age_ticks=pair(r,0x2c);s=rd(r,0x24);
 if(v.seq==before||v.seq!=rd(r,0x28)||!(s&PL_VALID_BIT)||(s&15)!=PL_DONE_BIT||v.age_ticks>max_age)return PL_STALE;
 if(v.schema!=0x10000)return PL_REJECTED;
 *out=v;return PL_OK;
}
