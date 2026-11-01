#include "runtime_control.h"
#include <string.h>
static uint32_t rd(RuntimeControl*c,uint32_t a){return c->regs.read32(c->regs.context,a);}
static bool wr(RuntimeControl*c,uint32_t a,uint32_t v){return c->regs.write32(c->regs.context,a,v)==0;}
static uint64_t pair(RuntimeControl*c,uint32_t a){uint64_t lo=rd(c,a);return lo|((uint64_t)rd(c,a+4)<<32);}
static void fail(RuntimeControl*c,PlResult r){c->failure=r;c->state=RC_FAILED;}
static void enter(RuntimeControl*c,RuntimeControlState s,uint64_t now){c->state=s;c->state_since=now;}
static bool expired(uint64_t now,uint64_t since){return now<since || now-since>=RUNTIME_CONTROL_LIMIT_US;}
static PlResult status(RuntimeControl*c,uint32_t a){
 uint32_t s=rd(c,a);if(s&PL_BUSY_BIT)return PL_BUSY;
 if(s&PL_TIMEOUT_BIT)return PL_TIMEOUT;
 if(s&PL_REJECTED_BIT)return PL_REJECTED;
 return s&PL_DONE_BIT?PL_OK:PL_BUSY;
}
bool runtime_control_init(RuntimeControl*c,const PlRegs*r,const RuntimeControlHooks*h){
 if(!c||!r||!h||!r->read32||!r->write32||!r->max_polls||!h->now_us||!h->init_receivers||!h->prepare_capture||!h->service_capture||
    !h->halt_flow||!h->idle_flow||!h->reset_begin||!h->reset_done||!h->rearm_flow||!h->fault_mask)return false;
 memset(c,0,sizeof(*c));c->regs=*r;c->hooks=*h;c->state=RC_WAIT;return true;
}
bool runtime_control_request_test(RuntimeControl*c,uint64_t s,uint32_t id){
 if(!c||c->state!=RC_WAIT||!s||!id)return false;
 memset(&c->config,0,sizeof(c->config));c->config.session_id=s;c->config.config_id=id;
 enter(c,RC_INIT,c->hooks.now_us(c->hooks.context));return true;
}
bool runtime_control_request_command(RuntimeControl*c,uint32_t command){
 if(!c||c->action_active||c->flow_active||c->fault_waiting||(c->state!=RC_RUN&&c->state!=RC_STOPPED)||(command!=PL_STOP&&command!=PL_RESET))return false;
 enter(c,command==PL_STOP?RC_USER_STOP_SEND:RC_RESET_SEND,c->hooks.now_us(c->hooks.context));return true;
}
/* The entire exact mask/epoch is ACKed only after each requested hardware
 * engine has independently provided completion proof. No submitted operation
 * qualifies as proof. Other flow remains serviceable during a local reset. */
static bool service_action(RuntimeControl*c,uint64_t now){
 PlDmaAction a;PlResult r=pl_dma_action_read(&c->regs,&a);
 if(c->action_ack_pending){
  r=status(c,0x14c);if(r==PL_BUSY){if(expired(now,c->action_since)){if(c->flow_active)c->flow_timed_out=true;fail(c,PL_TIMEOUT);}return false;}
  if(r!=PL_OK){fail(c,r);return false;}
  c->action_active=false;c->action_ack_pending=false;return true;
 }
 if(r==PL_EMPTY){if(c->action_active)fail(c,PL_STALE);return c->state!=RC_FAILED;}
 if(r!=PL_OK){fail(c,r);return false;}
 if(!c->action_active){c->action=a;c->action_active=true;c->action_since=now;c->action_started=0;c->action_done=0;}
 if(a.seq!=c->action.seq||a.mask!=c->action.mask||a.reset!=c->action.reset){fail(c,PL_STALE);return false;}
 if(expired(now,c->action_since)){
  fail(c,PL_TIMEOUT);
  if(!c->flow_active)return false;
  c->flow_timed_out=true; /* Continue only genuine pending ownership proof. */
 }
 if(c->flow_active && (a.mask!=c->flow_mask || !a.reset)){fail(c,PL_STALE);return false;}
 for(unsigned i=0;i<2;i++)if(a.mask&(1u<<i)){
  unsigned bit=1u<<i;
  if(!(c->action_started&bit)){
   bool ok=a.reset?c->hooks.reset_begin(c->hooks.context,i):c->hooks.halt_flow(c->hooks.context,i);
   if(!ok){fail(c,PL_REJECTED);return false;}c->action_started|=bit;
  }
  if(!(c->action_done&bit)){
   bool done=a.reset?c->hooks.reset_done(c->hooks.context,i):c->hooks.idle_flow(c->hooks.context,i);
   if(done){
    if(a.reset&&c->capture_ready&&c->state!=RC_RESET_WAIT&&!c->hooks.rearm_flow(c->hooks.context,i)){fail(c,PL_REJECTED);return false;}
    c->action_done|=bit;
   }
  }
 }
 if(c->action_done!=a.mask)return true;
 /* Re-read just before publishing completion; changed epoch/mask cannot use
  * proofs acquired for an earlier request. Adapter performs the same check. */
 r=pl_dma_action_read(&c->regs,&a);
 if(r!=PL_OK||a.seq!=c->action.seq||a.mask!=c->action.mask||a.reset!=c->action.reset){fail(c,PL_STALE);return false;}
 if(!wr(c,0x140,a.seq)||!wr(c,0x144,a.mask)||!wr(c,0x148,1)){fail(c,PL_BUS);return false;}
 c->action_ack_pending=true;return true;
}
static void service_flow(RuntimeControl*c,uint64_t now){
 if(c->flow_active){
  PlResult r=pl_flow_recover_poll(&c->regs,c->flow_seq);
  if(r==PL_TIMEOUT || expired(now,c->flow_since)){c->flow_timed_out=true;fail(c,PL_TIMEOUT);}
  if(r==PL_OK && !c->action_active){
   /* STATUS may retain timeout even after real completion. Do not convert a
    * timed-out operation into a clean RUN success. */
   if(rd(c,0x188)&PL_TIMEOUT_BIT){c->flow_timed_out=true;fail(c,PL_TIMEOUT);}
   c->flow_active=false;c->fault_waiting=false;
  }else if(r!=PL_BUSY && r!=PL_TIMEOUT && r!=PL_OK)fail(c,r);
  return;
 }
 if(c->state!=RC_RUN || c->action_active)return;
 uint32_t mask=c->hooks.fault_mask(c->hooks.context);
 if(mask&~3u){fail(c,PL_INVALID);return;}
 if(!mask){c->fault_waiting=false;return;}
 if(!c->fault_waiting){c->fault_waiting=true;c->fault_since=now;}
 if(expired(now,c->fault_since)){fail(c,PL_TIMEOUT);return;}
 /* Each FLOW request isolates one engine, even if both failed concurrently. */
 uint32_t flow=(mask&1)?1u:2u,seq=0;
 PlResult r=pl_flow_recover_request(&c->regs,flow,&seq);
 if(r==PL_BUSY)return;
 if(r!=PL_OK){fail(c,r);return;}
 c->flow_active=true;c->flow_seq=seq;c->flow_mask=flow;c->flow_since=now;c->flow_timed_out=false;
}
static bool begin_command(RuntimeControl*c,uint32_t command,RuntimeControlState next,uint64_t now){
 if((rd(c,0xdc)|rd(c,0xcc)|rd(c,0x188))&PL_BUSY_BIT)return true;
 if(!wr(c,8,command)){fail(c,PL_BUS);return false;}enter(c,next,now);return true;
}
void runtime_control_step(RuntimeControl*c){
 if(!c||c->state==RC_WAIT)return;
 uint64_t now=c->hooks.now_us(c->hooks.context);
 if(c->receivers_ready&&c->capture_ready)c->hooks.service_capture(c->hooks.context);
 if(c->state==RC_FAILED && !(c->failure==PL_TIMEOUT && c->flow_active))return;
 if(c->state!=RC_RUN&&c->state!=RC_STOPPED&&c->state!=RC_FAILED&&expired(now,c->state_since)){fail(c,PL_TIMEOUT);return;}
 if(c->receivers_ready&&!service_action(c,now))return;
 service_flow(c,now);
 PlResult r;
 switch(c->state){
 case RC_INIT:
  if(rd(c,0)!=UINT32_C(0x314e464c)||rd(c,4)!=1||!c->hooks.init_receivers(c->hooks.context)){fail(c,PL_REJECTED);break;}
  c->receivers_ready=true;enter(c,RC_STOP_SEND,now);break;
 case RC_STOP_SEND:(void)begin_command(c,PL_STOP,RC_STOP_WAIT,now);break;
 case RC_USER_STOP_SEND:(void)begin_command(c,PL_STOP,RC_USER_STOP_WAIT,now);break;
 case RC_RESET_SEND:(void)begin_command(c,PL_RESET,RC_RESET_WAIT,now);break;
 case RC_STOP_WAIT:case RC_USER_STOP_WAIT:case RC_RESET_WAIT:
  r=status(c,0xdc);if(r==PL_BUSY||c->action_active)break;if(r!=PL_OK){fail(c,r);break;}
  if(rd(c,0x18)!=0){fail(c,PL_REJECTED);break;}
  if(c->state==RC_STOP_WAIT)enter(c,RC_COMMIT_SEND,now);
  else {if(c->state==RC_RESET_WAIT)c->capture_ready=false;enter(c,RC_STOPPED,now);}break;
 case RC_COMMIT_SEND:
  if((rd(c,0xdc)|rd(c,0xcc)|rd(c,0x188))&PL_BUSY_BIT)break;
  if(!wr(c,0xc,PL_MODE_TEST_PATTERN)||!wr(c,0x10,c->config.config_id)||!wr(c,0x14,1)){fail(c,PL_BUS);break;}
  enter(c,RC_COMMIT_WAIT,now);break;
 case RC_COMMIT_WAIT:
  r=status(c,0xcc);if(r==PL_BUSY)break;if(r!=PL_OK){fail(c,r);break;}
  if(rd(c,0xc4)!=PL_MODE_TEST_PATTERN||rd(c,0xc8)!=c->config.config_id){fail(c,PL_REJECTED);break;}
  enter(c,RC_SNAPSHOT_SEND,now);break;
 case RC_SNAPSHOT_SEND:
  if(rd(c,0xd0)&PL_BUSY_BIT)break;
  c->snapshot_before=rd(c,0x30);
  if(!wr(c,0x2c,1)){fail(c,PL_BUS);break;}enter(c,RC_SNAPSHOT_WAIT,now);break;
 case RC_SNAPSHOT_WAIT:{
  r=status(c,0xd0);if(r==PL_BUSY)break;if(r!=PL_OK){fail(c,r);break;}
  uint32_t seq=rd(c,0x30),s=rd(c,0xd0);uint64_t age=UINT64_MAX;
  for(unsigned i=0;i<3;i++){
   uint32_t hi=rd(c,0xd8),lo=rd(c,0xd4),hi2=rd(c,0xd8);
   if(hi==hi2){age=((uint64_t)hi<<32)|lo;break;}
  }
  if(seq==c->snapshot_before||!(s&PL_VALID_BIT)||(s&PL_STALE_BIT)||age>5000000||rd(c,0xe0)!=1){fail(c,PL_STALE);break;}
  c->config.capture_id=pair(c,0x40);c->config.origin_raw=pair(c,0xf0);
  c->config.first_seq[0]=pair(c,0x158);c->config.first_seq[1]=pair(c,0x160);
  c->config.first_seq[2]=pair(c,0x170);c->config.first_seq[3]=pair(c,0x168);
  if(rd(c,0x30)!=seq||!c->config.capture_id||rd(c,0xc8)!=c->config.config_id||rd(c,0x18)!=0){fail(c,PL_STALE);break;}
  enter(c,RC_PREPARE,now);break;
 }
 case RC_PREPARE:
  if(!c->hooks.prepare_capture(c->hooks.context,&c->config)){fail(c,PL_REJECTED);break;}
  c->capture_ready=true;enter(c,RC_START_SEND,now);break;
 case RC_START_SEND:(void)begin_command(c,PL_START,RC_START_WAIT,now);break;
 case RC_START_WAIT:
  r=status(c,0xdc);if(r==PL_BUSY)break;if(r!=PL_OK){fail(c,r);break;}
  if(rd(c,0x18)!=RC_PL_WARMUP && rd(c,0x18)!=RC_PL_RUN){fail(c,PL_REJECTED);break;}enter(c,RC_RUN,now);break;
 default:break;
 }
}
