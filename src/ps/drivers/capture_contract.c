#include "capture_contract.h"
static int valid(const PlRegs *r){return r&&r->read32&&r->write32;}
static uint32_t rd(const PlRegs *r,uint32_t a){return r->read32(r->context,a);}
static uint64_t pair(const uint32_t *p){return p[0]|((uint64_t)p[1]<<32);}
PlResult pl_contract_probe(const PlRegs *r){
 if(!valid(r))return PL_INVALID;
 return rd(r,0)==0x31525443&&rd(r,4)==0x10000?PL_OK:PL_REJECTED;
}
PlResult pl_contract_read(const PlRegs *r,uint32_t kind,uint64_t session,uint64_t max_age,PlContractSnapshot *out){
 PlResult p;uint32_t before,w[27],age[2],status;PlContractSnapshot v={0};
 if(!out||!session||(kind!=1&&kind!=2))return PL_INVALID;
 p=pl_contract_probe(r);if(p!=PL_OK)return p;
 before=rd(r,0x10);if(before==UINT32_MAX)return PL_REJECTED;
 if(r->write32(r->context,8,kind))return PL_BUS;
 v.seq=rd(r,0x10);
 for(unsigned i=0;i<27;i++)w[i]=rd(r,0x80+i*4);
 age[0]=rd(r,0x14);age[1]=rd(r,0x18);v.age_ticks=pair(age);
 status=rd(r,0xc);
 if(v.seq!=before+1||rd(r,0x10)!=v.seq||status!=18||v.age_ticks>max_age)return PL_STALE;
 unsigned mask=w[1]==0?CAPTURE_MONITOR_MASK:CAPTURE_RAW_MASK;
 if(w[0]!=kind||w[1]>2||w[2]>1||w[3]>1||w[4]>3||!w[5]||!pair(w+6)||
    w[17]!=mask||(w[16]&~mask)||w[26]||(kind==1&&w[16]))return PL_REJECTED;
 v.kind=kind;v.mode=w[1];v.source=w[2];v.channel=w[3];v.pattern=w[4];v.bound_mask=w[16];
 v.plan.session_id=session;v.plan.capture_id=pair(w+6);v.plan.config_id=w[5];v.plan.raw_mode=w[1]!=0;
 for(unsigned i=0;i<4;i++){
  v.plan.first_seq[i]=pair(w+8+2*i);
  if((mask&(1u<<i))&&v.plan.first_seq[i]==UINT64_MAX)return PL_REJECTED;
  v.anchors[i]=(CaptureAnchor){session,v.plan.capture_id,v.plan.config_id,i+1,v.plan.first_seq[i],pair(w+18+2*i)};
  uint64_t span=i==0?UINT64_C(196608):i==2?UINT64_C(8192):UINT64_C(12288);
  if((v.bound_mask&(1u<<i))&&v.anchors[i].first_raw>UINT64_MAX-span)return PL_REJECTED;
  if(!(v.bound_mask&(1u<<i))&&v.anchors[i].first_raw)return PL_REJECTED;
 }
 *out=v;return PL_OK;
}
PlResult pl_contract_bind(CaptureTask *c,const PlContractSnapshot *s){
 if(!c||!s||s->kind!=PL_CONTRACT_ACTIVE||!c->explicit_contract)return PL_INVALID;
 unsigned mask=c->config.raw_mode?CAPTURE_RAW_MASK:CAPTURE_MONITOR_MASK;
 if(s->bound_mask&~mask||s->plan.session_id!=c->config.session_id||s->plan.capture_id!=c->config.capture_id||
    s->plan.config_id!=c->config.config_id||s->plan.raw_mode!=c->config.raw_mode)return PL_REJECTED;
 for(unsigned i=0;i<4;i++){
  if(s->plan.first_seq[i]!=c->config.first_seq[i])return PL_REJECTED;
  if((s->bound_mask&(1u<<i))&&!capture_task_check_anchor(c,&s->anchors[i]))return PL_REJECTED;
 }
 for(unsigned i=0;i<4;i++)if(s->bound_mask&(1u<<i))(void)capture_task_bind_anchor(c,&s->anchors[i]);
 return PL_OK;
}
