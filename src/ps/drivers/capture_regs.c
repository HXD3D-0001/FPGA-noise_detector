#include "capture_regs.h"
static int valid(const PlRegs*r){return r&&r->read32&&r->write32;}
static uint32_t rd(const PlRegs*r,uint32_t a){return r->read32(r->context,a);}
static uint64_t pair(const PlRegs*r,uint32_t a){uint64_t v=rd(r,a);return v|((uint64_t)rd(r,a+4)<<32);}
/* Age is live (unlike the latched payload). Bound retries on low-word carry. */
static int live_age(const PlRegs*r,uint64_t*out){
 for(unsigned i=0;i<3;i++){
  uint32_t high=rd(r,0x60),low=rd(r,0x5c);
  if(high==rd(r,0x60)){*out=((uint64_t)high<<32)|low;return 1;}
 }
 return 0;
}
PlResult pl_capture_probe(const PlRegs*r){
 if(!valid(r))return PL_INVALID;
 return rd(r,0)==0x31504143&&rd(r,4)==0x10000?PL_OK:PL_REJECTED;
}
static PlResult submit(const PlRegs*r,uint32_t address,uint32_t value,uint32_t channel,PlCaptureTicket*out){
 uint32_t a=channel?0x3c:0x10,before=rd(r,a);PlCaptureTicket t;
 if(before==UINT32_MAX)return PL_REJECTED;
 if(r->write32(r->context,address,value))return PL_BUS;
 /* SLVERR is silently swallowed by the GP write path, so a CAP1 rejected
  * submit (recorded as cs reason=2/busy) must be detected explicitly. */
 if((rd(r,0xc)&0x21)==0x21)return PL_REJECTED;
 t.channel=channel;t.seq=rd(r,a);
 if(t.seq!=before+1)return PL_STALE;
 *out=t;return PL_OK;
}
PlResult pl_capture_command_request(const PlRegs*r,uint32_t command,PlCaptureTicket*out){
 PlResult p;if(!out||command<1||command>4)return PL_INVALID;
 p=pl_capture_probe(r);if(p!=PL_OK)return p;
 if(rd(r,0x14)&1)return PL_BUSY;
 return submit(r,8,command,0,out);
}
PlResult pl_capture_commit_request(const PlRegs*r,const PlCaptureConfig*c,PlCaptureTicket*out){
 PlResult p;uint32_t values[5],i;
 if(!c||!out||c->mode>2||c->source>1||c->channel>1||c->pattern>3||!c->config_id)return PL_INVALID;
 p=pl_capture_probe(r);if(p!=PL_OK)return p;
 if(rd(r,0x14)&1)return PL_BUSY;
 if(rd(r,0x3c)==UINT32_MAX)return PL_REJECTED;
 values[0]=c->mode;values[1]=c->source;values[2]=c->channel;values[3]=c->pattern;values[4]=c->config_id;
 for(i=0;i<5;i++)if(r->write32(r->context,0x20+i*4,values[i]))return PL_BUS;
 return submit(r,0x34,1,1,out);
}
PlResult pl_capture_poll(const PlRegs*r,const PlCaptureTicket*t,uint32_t*out_status){
 PlResult p;uint32_t a,s;
 if(!t||t->channel>1||!t->seq)return PL_INVALID;
 p=pl_capture_probe(r);if(p!=PL_OK)return p;
 a=t->channel?0x3c:0x10;
 if(rd(r,a)!=t->seq)return PL_STALE;
 s=rd(r,t->channel?0x38:0xc);
 if(rd(r,a)!=t->seq)return PL_STALE;
 if(out_status)*out_status=s;
 if(s&PL_TIMEOUT_BIT)return PL_TIMEOUT;
 if(s&PL_BUSY_BIT)return PL_BUSY;
 if(s&PL_REJECTED_BIT)return PL_REJECTED;
 return s&PL_DONE_BIT?PL_OK:PL_STALE;
}
PlResult pl_capture_snapshot(const PlRegs*r,uint64_t max_age,PlCaptureSnapshot*out){
 PlCaptureSnapshot v;PlResult p;uint32_t before,s;
 if(!out)return PL_INVALID;
 p=pl_capture_probe(r);if(p!=PL_OK)return p;
 before=rd(r,0x58);if(before==UINT32_MAX)return PL_REJECTED;
 if(r->write32(r->context,0x50,1))return PL_BUS;
 v.seq=rd(r,0x58);v.flags=rd(r,0x80);
 v.configured.mode=rd(r,0x84);v.configured.source=rd(r,0x88);v.configured.channel=rd(r,0x8c);
 v.configured.pattern=rd(r,0x90);v.configured.config_id=rd(r,0x94);v.active_config_id=rd(r,0x98);
 v.capture_id=pair(r,0xa0);v.adc_count=pair(r,0xa8);v.test_count=pair(r,0xb0);
 v.latest_raw=pair(r,0xb8);v.origin_raw=pair(r,0xc0);
 if(!live_age(r,&v.age_ticks))return PL_STALE;
 s=rd(r,0x54);
 if(v.seq!=before+1||rd(r,0x58)!=v.seq||s!=18||v.age_ticks>max_age)return PL_STALE;
 *out=v;return PL_OK;
}
