#include "capture_runtime.h"
#include <string.h>
static bool regs_ok(const PlRegs *r){return r&&r->read32&&r->write32;}
static uint32_t rd(const PlRegs *r,uint32_t a){return r->read32(r->context,a);}
static bool config_ok(const PlCaptureConfig *p){return p&&p->mode<3&&p->source<2&&p->channel<2&&p->pattern<4&&p->config_id;}
static PlResult timed_snapshot(CaptureRuntime *c,PlCaptureSnapshot *s){
 uint64_t before=c->hooks.now_us(c->hooks.context);
 PlResult p=pl_capture_snapshot(&c->control,c->limits.snapshot_age_ticks,s);
 uint64_t after=c->hooks.now_us(c->hooks.context);
 if(p==PL_OK)(void)capture_time_pair_observe(&c->time_tracker,c->session_id,s,before,after);
 else c->time_tracker.have_previous=false;
 return p;
}
static void phase(CaptureRuntime *c,CaptureRuntimeState s,uint64_t now){c->state=s;c->since=now;c->phase_sent=false;}
static bool expired(uint64_t now,uint64_t since,uint64_t limit){return now<since||now-since>=limit;}
static void closing(CaptureRuntime *c,bool discard,uint64_t now){
 if(c->state!=CR_CLOSING){
  c->final_pending=false;c->final_clean=false;
  phase(c,CR_CLOSING,now);c->drain_done=false;c->reset_done=false;c->reset_failed=false;c->closures++;
  c->need_reset=discard;
  if(discard&&c->capture_ready)c->discard_closures++;
 }else if(discard&&!c->need_reset){c->need_reset=true;c->discard_closures++;c->final_pending=false;c->final_clean=false;}
}
static void fail(CaptureRuntime *c,PlResult p,uint64_t now){
 if(c->failure==PL_OK)c->failure=p;
 c->continue_start=false;c->quarantined=true;closing(c,true,now);
}
static void stale_read(CaptureRuntime *c,bool *waiting,uint64_t *since,uint64_t now){
 if(!*waiting){*waiting=true;*since=now;}
 if(expired(now,*since,c->limits.transaction_us))fail(c,PL_STALE,now);
}
static bool rings_free(const CaptureRuntime *c){
 for(unsigned f=0;f<2;f++){
  const DmaRx *r=c->owner->flows[f];if(r->resetting||r->fault)return false;
  for(size_t i=0;i<r->count;i++)if(r->slots[i].state!=DMA_FREE)return false;
 }
 return true;
}
static bool software_idle(CaptureRuntime *c){
 if(!c->capture_ready)return true;
 CaptureTask *t=c->capture;
 if(!c->hooks.analysis_idle(c->hooks.context)||t->energy_count||t->energy_pending_valid)return false;
 if(t->hooks.pool_lock&&!t->hooks.pool_lock(t->hooks.context))return false;
 bool empty=block_pool_free_count(t->pool)==LFN_BLOCK_SLOTS;
 if(t->hooks.pool_unlock)t->hooks.pool_unlock(t->hooks.context);
 return empty;
}
static bool plan_matches(const CaptureRuntime *c,const PlContractSnapshot *s){
 const PlCaptureConfig *r=&c->requested;
 return s->mode==r->mode&&s->source==r->source&&s->channel==r->channel&&s->pattern==r->pattern&&s->plan.config_id==r->config_id&&s->plan.session_id==c->session_id;
}
static bool task_matches(const CaptureRuntime *c,const CapturePlan *p){
 const CaptureTask *t=c->capture;
 if(!t->explicit_contract||t->flows[0]!=c->owner->flows[0]||t->flows[1]!=c->owner->flows[1]||
    t->config.session_id!=p->session_id||t->config.capture_id!=p->capture_id||t->config.config_id!=p->config_id||t->config.raw_mode!=p->raw_mode)return false;
 for(unsigned i=0;i<4;i++)if(t->config.first_seq[i]!=p->first_seq[i])return false;
 return true;
}
static void watch(CaptureRuntime *c){
 memcpy(c->reject_watch,c->capture->rejects,sizeof(c->reject_watch));c->energy_watch=c->capture->energy_rejected;
}
static bool rejected(const CaptureRuntime *c){
 if(c->energy_watch!=c->capture->energy_rejected)return true;
 for(unsigned i=0;i<CAPTURE_REASON_COUNT;i++)if(i!=CAPTURE_POOL_FULL&&i!=CAPTURE_POOL_BUSY&&c->reject_watch[i]!=c->capture->rejects[i])return true;
 return false;
}
bool capture_runtime_init(CaptureRuntime *c,const PlRegs *control,const PlRegs *contract,CaptureTask *task,CaptureDataOwner *owner,const CaptureRuntimeHooks *h,const CaptureRuntimeLimits *l){
 if(!c||!regs_ok(control)||!regs_ok(contract)||!task||!owner||!regs_ok(&owner->regs)||!owner->flows[0]||!owner->flows[1]||owner->active||
    !h||!h->now_us||!h->prepare||!h->analysis_step||!h->analysis_idle||!h->fault_mask||
    !l||!l->transaction_us||!l->bind_us||!l->close_us||!l->snapshot_age_ticks)return false;
 memset(c,0,sizeof(*c));c->control=*control;c->contract=*contract;c->capture=task;c->owner=owner;c->hooks=*h;c->limits=*l;c->state=CR_WAIT;return true;
}
bool capture_runtime_request_start(CaptureRuntime *c,uint64_t session,const PlCaptureConfig *r){
 if(!c||!session||!config_ok(r)||(c->state!=CR_WAIT&&c->state!=CR_STOPPED&&c->state!=CR_FAILED)||c->cap_pending||c->data_pending||c->owner->active||c->uncertain)return false;
 c->requested=*r;c->session_id=session;c->failure=PL_OK;c->quarantined=false;c->continue_start=true;
 c->final_pending=false;c->final_clean=false;
 capture_time_pair_init(&c->time_tracker);
 c->state=CR_PROBE;c->have_time=false;c->cap_stale=false;c->contract_stale=false;return true;
}
bool capture_runtime_request_stop(CaptureRuntime *c,bool discard){
 if(!c||c->state==CR_WAIT||c->state==CR_STOPPED||c->state==CR_FAILED||c->state==CR_INCOMPATIBLE||c->state==CR_QUARANTINED)return false;
 c->continue_start=false;
 /* Clock is sampled by step; don't call platform hooks from request paths. */
 if(c->state==CR_PROBE){c->need_reset=true;return true;}
 closing(c,discard||c->state!=CR_RUN,c->last_now);return true;
}
static void poll_cap(CaptureRuntime *c,uint64_t now){
 if(!c->cap_pending)return;
 uint32_t s=0;PlResult p=pl_capture_poll(&c->control,&c->cap_ticket,&s);
 if(p==PL_STALE||p==PL_BUS||p==PL_INVALID||(p==PL_REJECTED&&!s)){
  c->uncertain=true;fail(c,p,now);return;
 }
 if(p==PL_TIMEOUT)fail(c,p,now);
 if(s&PL_BUSY_BIT)return;
 if(!(s&PL_DONE_BIT)){c->uncertain=true;fail(c,PL_STALE,now);return;}
 c->cap_pending=false;
 if(p!=PL_OK)fail(c,p,now);
}
static void poll_data(CaptureRuntime *c,uint64_t now){
 if(!c->data_pending)return;
 uint32_t s=0;PlResult p=pl_data_poll(&c->owner->regs,c->data_ticket,&s);
 if(p==PL_STALE||p==PL_BUS||p==PL_INVALID||(p==PL_REJECTED&&!s)){
  c->uncertain=true;fail(c,p,now);return;
 }
 if(p==PL_TIMEOUT)fail(c,p,now);
 if(s&PL_BUSY_BIT)return;
 if(!(s&PL_DONE_BIT)){c->uncertain=true;fail(c,PL_STALE,now);return;}
 c->data_pending=false;
 if(p!=PL_OK&&p!=PL_TIMEOUT){c->reset_failed=c->data_kind==PL_DATA_RESET;fail(c,p,now);return;}
 if(c->data_kind==PL_DATA_DRAIN)c->drain_done=true;
 else if(c->data_kind==PL_DATA_RESET)c->reset_done=true;
}
static bool drain_received(CaptureRuntime *c){
 if(c->owner->active||!c->capture_ready||c->failure!=PL_OK||c->data_kind!=PL_DATA_DRAIN)return true;
 PlDataAction action;
 if(pl_data_action(&c->owner->regs,&action)!=PL_OK)return false;
 if(action.reset||action.resume||action.mask!=3||c->drain_action_before==UINT32_MAX||
    action.seq!=c->drain_action_before+1||rd(&c->owner->regs,0x10)!=c->data_ticket)return false;
 /* The exact DRAIN action proves PL is empty. Read a fresh final position,
  * since the previous scheduler snapshot may precede its last accepted beat.
  * Continue consuming DMA while waiting; close_us still bounds this wait. */
 PlCaptureSnapshot s;
 if(timed_snapshot(c,&s)!=PL_OK||!(s.flags&PL_CAPTURE_F_POSITION_VALID)||
    s.capture_id!=c->capture->config.capture_id||s.active_config_id!=c->capture->config.config_id||
    c->capture->config.session_id!=c->session_id||
    ((s.flags&PL_CAPTURE_F_TEST)!=0)!=(c->requested.source==PL_CAPTURE_TEST)||
    !capture_task_received_through(c->capture,s.latest_raw))return false;
 c->final_capture=s;c->final_session=c->session_id;c->final_drain_seq=action.seq;
 c->final_drain_ticket=c->data_ticket;c->final_pending=true;return true;
}
static void close_step(CaptureRuntime *c,const PlCaptureSnapshot *s,uint64_t now){
 unsigned frontend=(s->flags>>16)&15;bool off=frontend==0||frontend==6||frontend==7;
 /* A real backend fault makes the DRAIN idle proof impossible (a faulted
  * backend never reports idle). Escalate to the reset-based failure path;
  * never claim a clean STOPPED after a fault. If a non-reset DAT1 action is
  * already in flight the PL cannot accept the replacing RESET before that
  * action resolves, so expose the terminal quarantined state instead of
  * spinning forever in CR_CLOSING. */
 if(c->hooks.fault_mask(c->hooks.context)){
  c->continue_start=false;c->quarantined=true;c->need_reset=true;
  if(c->failure==PL_OK)c->failure=PL_REJECTED;
  if(c->data_pending&&c->data_kind!=PL_DATA_RESET){phase(c,CR_QUARANTINED,now);return;}
 }
 if(expired(now,c->since,c->limits.close_us)){if(c->failure==PL_OK)c->failure=PL_TIMEOUT;c->continue_start=false;c->quarantined=true;}
 if(c->capture_ready&&capture_task_pending_anchors(c->capture))c->need_reset=true;
 if(c->uncertain)return; /* An ambiguous command write/tag needs platform recovery. */
 if(!off&&!c->cap_pending){
  PlResult p=pl_capture_command_request(&c->control,PL_CAPTURE_STOP,&c->cap_ticket);
  if(p==PL_OK){c->cap_pending=true;c->cap_command=PL_CAPTURE_STOP;}
  else if(p!=PL_BUSY){c->uncertain=true;fail(c,p,now);return;}
 }
 /* STOPPED can be a transient view before an issued START is accepted.
  * A pending START/commit must finish or prove STOP/RECOVER/DRAIN/FAULT before
  * resetting DMA; never let a delayed START overlap an ownership reset. */
 unsigned operation=(s->flags>>9)&7;
 bool stable_off=off&&(!c->cap_pending||c->cap_command==PL_CAPTURE_STOP||frontend==6||frontend==7||
   operation==PL_CAPTURE_OP_STOP||operation==PL_CAPTURE_OP_RECOVER);
 if(stable_off&&!c->data_pending&&!c->owner->active&&!c->reset_done&&!c->reset_failed){
  uint32_t kind=(!c->need_reset&&!c->drain_done)?PL_DATA_DRAIN:PL_DATA_RESET;
  uint32_t action_before=rd(&c->owner->regs,0x18);
  PlResult p=pl_data_request(&c->owner->regs,kind,&c->data_ticket);
  if(p==PL_OK){c->data_kind=kind;c->data_pending=true;if(kind==PL_DATA_DRAIN)c->drain_action_before=action_before;}
  else if(p!=PL_BUSY){c->uncertain=true;fail(c,p,now);return;}
 }
 if(!c->reset_done||c->data_pending||c->owner->active||c->cap_pending||
    !(s->flags&PL_CAPTURE_F_STOP_COMPLETE)||(s->flags&(PL_CAPTURE_F_SERVICE_BUSY|PL_CAPTURE_F_CLOCK_BUSY|PL_CAPTURE_F_RUNNING))||
    (rd(&c->control,0x14)&1)||(rd(&c->owner->regs,0x14)&7)!=4||!rings_free(c)||!software_idle(c))return;
 c->quarantined=false;
 if(c->failure!=PL_OK){phase(c,CR_FAILED,now);return;}
 c->final_clean=!c->continue_start&&!c->need_reset&&c->drain_done&&c->final_pending;
 if(c->continue_start)phase(c,CR_COMMIT,now);else phase(c,CR_STOPPED,now);
}
void capture_runtime_step(CaptureRuntime *c){
 if(!c||c->state==CR_WAIT||c->state==CR_STOPPED||c->state==CR_FAILED||c->state==CR_INCOMPATIBLE||c->state==CR_QUARANTINED)return;
 uint64_t now=c->hooks.now_us(c->hooks.context);
 bool backwards=c->have_time&&now<c->last_now;c->last_now=now;
 if(!c->have_time){c->have_time=true;c->since=now;}
 if(c->state==CR_PROBE){
  PlResult p=pl_capture_probe(&c->control);if(p==PL_OK)p=pl_data_probe(&c->owner->regs);if(p==PL_OK)p=pl_contract_probe(&c->contract);
  if(p!=PL_OK){c->failure=p;c->quarantined=true;c->state=CR_INCOMPATIBLE;return;}
  c->verified=true;closing(c,true,now);
  if((rd(&c->control,0x14)&1)||(rd(&c->owner->regs,0x14)&3)||(rd(&c->owner->regs,0x1c)&3)){
   c->uncertain=true;fail(c,PL_BUSY,now);
  }
 }
 if(backwards)fail(c,PL_TIMEOUT,now);
 /* Always service bounded consumers and the real ownership adapter while an
  * operation waits. No IRQ callback drains or reclaims these buffers. */
 if(c->capture_ready){
  (void)capture_task_run(c->capture);c->hooks.analysis_step(c->hooks.context);
  if(rejected(c))fail(c,PL_REJECTED,now);
 }
 // Validate command ownership before starting any new DMA action. An old
 // or foreign DAT1 request is not ours merely because its action is readable.
 poll_cap(c,now);poll_data(c,now);
 PlResult owner=PL_EMPTY;
 if(c->owner->active||(c->data_pending&&!c->uncertain))owner=drain_received(c)?capture_data_owner_step(c->owner,now):PL_BUSY;
 else if(!c->uncertain){
  PlDataAction action;PlResult pending=pl_data_action(&c->owner->regs,&action);
  if(pending==PL_OK||(rd(&c->owner->regs,0x14)&3)){
   c->uncertain=true;fail(c,PL_BUSY,now);
  }else if(pending!=PL_EMPTY){c->uncertain=true;fail(c,pending,now);}
 }
 if(owner!=PL_OK&&owner!=PL_EMPTY&&owner!=PL_BUSY)fail(c,owner,now);
 // Phase/close deadlines keep running even if a snapshot is unavailable.
 if(c->state!=CR_RUN&&c->state!=CR_CLOSING&&expired(now,c->since,c->state==CR_BIND?c->limits.bind_us:c->limits.transaction_us))fail(c,PL_TIMEOUT,now);
 if(c->state==CR_CLOSING&&expired(now,c->since,c->limits.close_us)){
  if(c->failure==PL_OK)c->failure=PL_TIMEOUT;
  c->continue_start=false;c->quarantined=true;
 }
 PlCaptureSnapshot snap;PlResult p=timed_snapshot(c,&snap);
 if(p!=PL_OK){
  if(p==PL_STALE)stale_read(c,&c->cap_stale,&c->cap_stale_since,now);else fail(c,p,now);
  return;
 }
 c->cap_stale=false;c->observed_capture=snap;c->have_observed_capture=true;
 if((snap.flags&(PL_CAPTURE_F_RUNNING|PL_CAPTURE_F_POSITION_VALID))==(PL_CAPTURE_F_RUNNING|PL_CAPTURE_F_POSITION_VALID)&&snap.capture_id==c->prepared.plan.capture_id&&snap.active_config_id==c->requested.config_id&&(!c->have_observed_started||c->observed_started.capture_id!=snap.capture_id)){c->observed_started=snap;c->have_observed_started=true;}
 if(c->state!=CR_CLOSING&&((snap.flags&PL_CAPTURE_F_QUARANTINED)||((snap.flags>>16)&15)==7||c->hooks.fault_mask(c->hooks.context)))fail(c,PL_REJECTED,now);
 if(c->state!=CR_RUN&&c->state!=CR_CLOSING&&expired(now,c->since,c->state==CR_BIND?c->limits.bind_us:c->limits.transaction_us))fail(c,PL_TIMEOUT,now);
 switch(c->state){
 case CR_CLOSING:close_step(c,&snap,now);break;
 case CR_COMMIT:
  if(!c->phase_sent){
   p=pl_capture_commit_request(&c->control,&c->requested,&c->cap_ticket);
   if(p==PL_OK){c->cap_pending=true;c->cap_command=0;c->phase_sent=true;}
   else if(p!=PL_BUSY){c->uncertain=p==PL_BUS||p==PL_STALE;fail(c,p,now);}
  }else if(!c->cap_pending)phase(c,CR_PLAN,now);
  break;
 case CR_PLAN:{
  PlContractSnapshot meta;p=pl_contract_read(&c->contract,PL_CONTRACT_PLAN,c->session_id,c->limits.snapshot_age_ticks,&meta);
  if(p==PL_STALE){stale_read(c,&c->contract_stale,&c->contract_stale_since,now);break;}
  c->contract_stale=false;
  if(p!=PL_OK||!plan_matches(c,&meta)){fail(c,p==PL_OK?PL_REJECTED:p,now);break;}
  bool reuse=c->capture_ready&&task_matches(c,&meta.plan)&&capture_task_pending_anchors(c->capture)==(meta.plan.raw_mode?CAPTURE_RAW_MASK:CAPTURE_MONITOR_MASK);
  if(!rings_free(c)||!software_idle(c)||(!reuse&&!c->hooks.prepare(c->hooks.context,&meta.plan))||!task_matches(c,&meta.plan)||
     capture_task_pending_anchors(c->capture)!=(meta.plan.raw_mode?CAPTURE_RAW_MASK:CAPTURE_MONITOR_MASK)){
   fail(c,PL_REJECTED,now);break;
  }
  c->capture_ready=true;c->prepared=meta;watch(c);c->arm_flow=0;phase(c,CR_ARM,now);break;
 }
 case CR_ARM:
  if(!c->owner->hooks.rearm(c->owner->hooks.context,c->arm_flow)){fail(c,PL_REJECTED,now);break;}
  if(c->arm_flow==0&&!c->requested.mode)c->arm_flow=1;else phase(c,CR_START,now);
  break;
 case CR_START:
  if(!c->phase_sent){
   p=pl_capture_command_request(&c->control,PL_CAPTURE_START,&c->cap_ticket);
   if(p==PL_OK){c->cap_pending=true;c->cap_command=PL_CAPTURE_START;c->phase_sent=true;}
   else if(p!=PL_BUSY){c->uncertain=p==PL_BUS||p==PL_STALE;fail(c,p,now);}
  }else if(!c->cap_pending)phase(c,CR_BIND,now);
  break;
 case CR_BIND:case CR_RUN:{
  if(snap.capture_id!=c->prepared.plan.capture_id||snap.active_config_id!=c->requested.config_id||
     !(snap.flags&PL_CAPTURE_F_RUNNING)||((snap.flags&PL_CAPTURE_F_TEST)!=0)!=(c->requested.source!=0)||(snap.flags&PL_CAPTURE_F_QUARANTINED)){
   fail(c,PL_REJECTED,now);break;
  }
  PlContractSnapshot meta;p=pl_contract_read(&c->contract,PL_CONTRACT_ACTIVE,c->session_id,c->limits.snapshot_age_ticks,&meta);
  if(p==PL_STALE){stale_read(c,&c->contract_stale,&c->contract_stale_since,now);break;}
  c->contract_stale=false;
  if(p!=PL_OK||!plan_matches(c,&meta)||pl_contract_bind(c->capture,&meta)!=PL_OK){fail(c,p==PL_OK?PL_REJECTED:p,now);break;}
  c->observed_active=meta;c->have_observed_active=true;
  if(c->state==CR_BIND&&!capture_task_pending_anchors(c->capture))phase(c,CR_RUN,now);
  break;
 }
 default:fail(c,PL_INVALID,now);break;
 }
}
