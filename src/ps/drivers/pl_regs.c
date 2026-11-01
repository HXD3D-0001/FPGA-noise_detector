#include "pl_regs.h"
#include <stddef.h>
static int valid(const PlRegs *r){return r&&r->read32&&r->write32&&r->max_polls;}
static uint32_t read_reg(const PlRegs*r,uint32_t a){return r->read32(r->context,a);}
static PlResult write_reg(const PlRegs*r,uint32_t a,uint32_t v){return r->write32(r->context,a,v)?PL_BUS:PL_OK;}
static uint64_t pair(const PlRegs*r,uint32_t a){uint64_t lo=read_reg(r,a);return lo|((uint64_t)read_reg(r,a+4)<<32);}
static PlResult completion(const PlRegs*r,uint32_t address){
 uint32_t i;
 for(i=0;i<r->max_polls;i++){
  uint32_t status=read_reg(r,address);
  if(!(status&PL_BUSY_BIT)){
   if(status&PL_TIMEOUT_BIT)return PL_TIMEOUT;
   if(status&PL_REJECTED_BIT)return PL_REJECTED;
   if(status&PL_DONE_BIT)return PL_OK;
  }
  if(r->poll_wait)r->poll_wait(r->context);
 }
 return PL_TIMEOUT;
}
PlResult pl_command(const PlRegs*r,uint32_t command){
 PlResult result;
 if(!valid(r)||(command!=PL_START&&command!=PL_STOP&&command!=PL_RESET))return PL_INVALID;
 if((read_reg(r,0xdc)|read_reg(r,0xcc)|read_reg(r,0x188))&PL_BUSY_BIT)return PL_BUSY;
 result=write_reg(r,8,command);if(result!=PL_OK)return result;
 return completion(r,0xdc);
}
PlResult pl_commit(const PlRegs*r,uint32_t mode,uint32_t config_id){
 PlResult result;
 if(!valid(r)||mode>3||config_id==0)return PL_INVALID;
 if(read_reg(r,0x18)!=0)return PL_REJECTED;
 if((read_reg(r,0xdc)|read_reg(r,0xcc)|read_reg(r,0x188))&PL_BUSY_BIT)return PL_BUSY;
 result=write_reg(r,0xc,mode);if(result!=PL_OK)return result;
 result=write_reg(r,0x10,config_id);if(result!=PL_OK)return result;
 result=write_reg(r,0x14,1);if(result!=PL_OK)return result;
 result=completion(r,0xcc);if(result!=PL_OK)return result;
 if(read_reg(r,0xc4)!=mode||read_reg(r,0xc8)!=config_id)return PL_REJECTED;
 return PL_OK;
}
PlResult pl_snapshot(const PlRegs*r,uint64_t max_age_ticks,PlSnapshot*out){
 PlSnapshot value;PlResult result;uint32_t before,status,i;
 if(!valid(r)||!out)return PL_INVALID;
 if(read_reg(r,0xd0)&PL_BUSY_BIT)return PL_BUSY;
 before=read_reg(r,0x30);
 result=write_reg(r,0x2c,1);if(result!=PL_OK)return result;
 result=completion(r,0xd0);if(result!=PL_OK)return result;
 value.seq=read_reg(r,0x30);
 if(value.seq==before)return PL_STALE;
 for(i=0;i<8;i++)value.counters[i]=pair(r,0x40+i*8);
 value.counters[8]=pair(r,0xf8);
 for(i=9;i<16;i++)value.counters[i]=pair(r,0x100+(i-9)*8);
 value.source_id=read_reg(r,0xe0);value.source_position_valid=read_reg(r,0xe4);
 value.latest_raw=pair(r,0xe8);value.origin_raw=pair(r,0xf0);
 value.energy_flush_count=pair(r,0x150);value.pcm_flush_count=pair(r,0x178);
 value.fft_first_seq=pair(r,0x158);value.pcm_first_seq=pair(r,0x160);
 value.energy_first_seq=pair(r,0x168);value.raw_first_seq=pair(r,0x170);
 /* Age is a live 64-bit counter, unlike the frozen snapshot. Read high-low-high
  * with bounded retries so a low-word carry cannot tear it. */
 for(i=0;i<3;i++){
  uint32_t high=read_reg(r,0xd8),low=read_reg(r,0xd4),high2=read_reg(r,0xd8);
  if(high==high2){value.age_ticks=((uint64_t)high<<32)|low;break;}
 }
 if(i==3)return PL_STALE;
 status=read_reg(r,0xd0);
 if(!(status&PL_VALID_BIT)||status&(PL_TIMEOUT_BIT|PL_STALE_BIT)||value.age_ticks>max_age_ticks||read_reg(r,0x30)!=value.seq)return PL_STALE;
 *out=value;return PL_OK;
}
PlResult pl_dma_action_read(const PlRegs*r,PlDmaAction*out){
 unsigned i;PlDmaAction value;
 if(!valid(r)||!out)return PL_INVALID;
 for(i=0;i<3;i++){
  uint32_t seq=read_reg(r,0x138),flags=read_reg(r,0x13c);
  if(seq!=read_reg(r,0x138))continue;
  if((flags&3)==0)return PL_EMPTY;
  if(flags&~UINT32_C(0x103))return PL_INVALID;
  value.seq=seq;value.mask=flags&3;value.reset=(flags&0x100)!=0;*out=value;return PL_OK;
 }
 return PL_STALE;
}
PlResult pl_dma_action_ack(const PlRegs*r,uint32_t seq,uint32_t mask){
 PlDmaAction value;PlResult result;
 if(mask==0||mask>3)return PL_INVALID;
 result=pl_dma_action_read(r,&value);if(result!=PL_OK)return result;
 if(value.seq!=seq)return PL_STALE;
 if(value.mask!=mask)return PL_REJECTED;
 result=write_reg(r,0x140,seq);if(result!=PL_OK)return result;
 result=write_reg(r,0x144,mask);if(result!=PL_OK)return result;
 result=write_reg(r,0x148,1);if(result!=PL_OK)return result;
 return completion(r,0x14c);
}
PlResult pl_energy_read(const PlRegs*r,PlEnergy*out){
 PlEnergy value;PlResult result;
 if(!valid(r)||!out)return PL_INVALID;
 /* Resume an existing lock after a transient MMIO error; only capture_task may
  * own this adapter, and a successful POP invalidates it exactly once. */
 if(!read_reg(r,0xb0)){
  if(read_reg(r,0x34)==0)return PL_EMPTY;
  result=write_reg(r,0x80,1);if(result!=PL_OK)return result;
 }
 if(!read_reg(r,0xb0))return PL_REJECTED;
 value.seq=pair(r,0x88);value.capture_id=pair(r,0x90);value.first_raw=pair(r,0x98);
 value.energy=pair(r,0xa0);value.config_id=read_reg(r,0xa8);value.flags=read_reg(r,0xac);
 if(!read_reg(r,0xb0)||pair(r,0x88)!=value.seq)return PL_STALE;
 result=write_reg(r,0xbc,(uint32_t)value.seq);if(result!=PL_OK)return result;
 result=write_reg(r,0xc0,(uint32_t)(value.seq>>32));if(result!=PL_OK)return result;
 result=write_reg(r,0x84,1);if(result!=PL_OK)return result;
 if(read_reg(r,0xb0))return PL_REJECTED;
 *out=value;return PL_OK;
}

PlResult pl_flow_recover_request(const PlRegs*r,uint32_t mask,uint32_t*seq){
 PlResult result;uint32_t before,status;
 if(!valid(r)||!seq||(mask!=1&&mask!=2))return PL_INVALID;
 if((read_reg(r,0xdc)|read_reg(r,0xcc)|read_reg(r,0x188))&PL_BUSY_BIT)return PL_BUSY;
 before=read_reg(r,0x18c);if(before==UINT32_MAX)return PL_REJECTED;
 result=write_reg(r,0x180,mask);if(result!=PL_OK)return result;
 result=write_reg(r,0x184,1);if(result!=PL_OK)return result;
 status=read_reg(r,0x188);
 if(read_reg(r,0x18c)!=before+1||!(status&PL_BUSY_BIT))return PL_REJECTED;
 *seq=before+1;return PL_OK;
}
PlResult pl_flow_recover_poll(const PlRegs*r,uint32_t seq){
 uint32_t status;
 if(!valid(r)||!seq)return PL_INVALID;
 if(read_reg(r,0x18c)!=seq)return PL_STALE;
 status=read_reg(r,0x188);
 if(status&PL_BUSY_BIT)return status&PL_TIMEOUT_BIT?PL_TIMEOUT:PL_BUSY;
 if(status&PL_REJECTED_BIT)return PL_REJECTED;
 if(status&PL_DONE_BIT)return PL_OK;
 return status&PL_TIMEOUT_BIT?PL_TIMEOUT:PL_STALE;
}
