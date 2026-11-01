#include "capture_application.h"
#include <string.h>
static uint64_t us(void *v){CaptureApplication*a=v;return a->hooks.now_us(a->hooks.context);}
static uint64_t ns(void *v){uint64_t t=us(v);return t>UINT64_MAX/1000?UINT64_MAX:t*1000;}
static void notify(void *v){CaptureApplication*a=v;a->hooks.notify(a->hooks.context);}
static bool energy(void *v,PlEnergy*out){
 CaptureApplication*a=v;PlResult p=pl_data_energy_read(&a->dat,out);
 if(p!=PL_OK&&p!=PL_EMPTY)a->soft_fault|=UINT32_C(0x80000000);
 return p==PL_OK;
}
static bool raw_current(const CaptureApplication*a){
 return a->raw&&a->prepared&&a->plan_valid&&a->active_config.mode&&a->raw->status.prepared&&
 a->raw->status.session_id==a->task.config.session_id&&a->raw->status.capture_id==a->task.config.capture_id;
}
static void packet(void*v,const PacketView*p){
 CaptureApplication*a=v;if(p->header.type>=1&&p->header.type<=3)a->packets[p->header.type-1]++;
 if(a->history&&p->header.type==LFN_PCM)(void)history_append_packet(a->history,a->task.config.session_id,p);
 if(a->storage&&(p->header.type==LFN_PCM||p->header.type==LFN_RAW)){
  LfnServiceIdentity id={a->task.config.session_id,a->task.config.capture_id,a->active_config};(void)capture_storage_route_packet(a->storage,&id,p);
 }
 /* CaptureTask supplies a validated decoder view of contiguous header/payload;
  * copy before this synchronous callback returns the borrowed DMA memory. */
 if(p->header.type==LFN_RAW&&raw_current(a))
  (void)lfn_raw_offer(a->raw,p->payload-LFN_HEADER_BYTES,p->payload_bytes+LFN_HEADER_BYTES,us(a));
}
static void packet_reject(void*v,CaptureReject reason,const PacketHeader*h){
 CaptureApplication*a=v;(void)reason;(void)h;if(raw_current(a))lfn_raw_stop(a->raw,RAW_STOP_INPUT_LOSS);
}
static void packet_gap(void*v,uint16_t type,uint64_t first,uint64_t end){
 CaptureApplication*a=v;(void)first;(void)end;if(type==LFN_RAW&&raw_current(a))lfn_raw_stop(a->raw,RAW_STOP_INPUT_LOSS);
}
static bool prepare(void*v,const CapturePlan*p){
 CaptureApplication*a=v;a->plan_valid=false;a->accepted_capture=false;CaptureHooks ch={.context=a,.monotonic_us=us,.notify_task=notify,.on_packet=packet,.on_reject=packet_reject,.on_gap=packet_gap,.energy_read=energy};
 bool ok=a->prepared?capture_task_prepare(&a->task,p):capture_task_init_plan(&a->task,a->flows[0],a->flows[1],a->pool,p,&ch);
 if(!ok)return false;
 if(a->history&&!history_begin(a->history,(HistoryIdentity){p->session_id,p->capture_id,p->config_id},!p->raw_mode))return false;
 LfnAnalysisConfig config={p->config_id,1000,0,false,a->selected.source==PL_CAPTURE_TEST};
 LfnAnalysisHooks ah={.context=a,.monotonic_ns=ns};
 if(!lfn_analysis_init(&a->analysis,a->pool,p->session_id,p->capture_id,&config,&ah))return false;
 if(a->selected.mode&&!lfn_raw_begin(a->raw,p->session_id,p->capture_id,p->config_id,&a->limits.raw))return false;
 a->active_config=a->selected;a->accepted_capture=false;a->prepared=true;a->plan_valid=true;return true;
}
static void analysis(void*v){
 CaptureApplication*a=v;(void)lfn_analysis_step(&a->analysis);
 for(unsigned i=0;i<CAPTURE_ENERGY_PER_ROUND;i++){
  CaptureEnergyMessage m;if(!capture_energy_take(&a->task,&m))break;
  a->last_energy=m;if(m.gap)a->energy_gaps++;else a->energy_records++;
  if(a->services){
   LfnServiceIdentity identity={a->task.config.session_id,a->task.config.capture_id,a->active_config};
   (void)lfn_service_energy_offer(a->services,&identity,&m);
  }
 }
}
static bool idle_analysis(void*v){CaptureApplication*a=v;return !a->prepared||(!a->analysis.pending_release&&block_pool_free_count(a->pool)==LFN_BLOCK_SLOTS);}
static uint32_t fault(void*v){CaptureApplication*a=v;return a->soft_fault|a->hooks.fault_mask(a->hooks.context);}
static bool halt(void*v,unsigned f){CaptureApplication*a=v;return a->hooks.halt(a->hooks.context,f);}
static bool idle(void*v,unsigned f){CaptureApplication*a=v;return a->hooks.idle(a->hooks.context,f);}
static bool rearm(void*v,unsigned f){CaptureApplication*a=v;return a->hooks.rearm(a->hooks.context,f);}
static bool regs(const PlRegs*r){return r&&r->read32&&r->write32;}
static void fail(CaptureApplication*a,PlResult p){a->failure=p;a->state=CA_FAULT;}
bool capture_application_init(CaptureApplication*a,const PlRegs*cap,const PlRegs*dat,const PlRegs*ctr,
 DmaRx*x,DmaRx*y,BlockPool*pool,const CaptureApplicationHooks*h){
 if(!a||!regs(cap)||!regs(dat)||!regs(ctr)||!x||!y||x==y||!pool||!h||!h->now_us||!h->notify||!h->bind_dma||!h->halt||!h->idle||!h->rearm||!h->fault_mask)return false;
 memset(a,0,sizeof(*a));a->cap=*cap;a->dat=*dat;a->ctr=*ctr;a->flows[0]=x;a->flows[1]=y;a->pool=pool;a->hooks=*h;return true;
}
bool capture_application_request_start(CaptureApplication*a,uint64_t session,const PlCaptureConfig*c,const CaptureApplicationLimits*l){
 if(!a||a->storage_close_requested||a->storage_input_closed||(a->storage&&session!=a->storage->session_id)||!session||!c||c->mode>2||c->source>1||c->channel>1||c->pattern>3||!c->config_id||!l||!l->owner_us||
    !l->runtime.transaction_us||!l->runtime.bind_us||!l->runtime.close_us||!l->runtime.snapshot_age_ticks)return false;
 if(c->mode? !lfn_raw_policy_valid(a->raw,&l->raw):(l->raw.duration_us||l->raw.capacity_bytes))return false;
 if(a->state!=CA_WAIT&&!(a->state==CA_ACTIVE&&(a->runtime.state==CR_STOPPED||a->runtime.state==CR_FAILED)))return false;
 if(a->clock_pending)return false;
 if(a->bound&&!capture_runtime_request_start(&a->runtime,session,c))return false;
 a->selected=*c;a->limits=*l;a->session=session;a->failure=PL_OK;a->soft_fault=0;
 if(a->bound){a->runtime.limits=l->runtime;a->owner.limit_us=l->owner_us;return true;}
 a->have_time=false;a->cancel_start=false;a->state=CA_PROBE;return true;
}
bool capture_application_request_stop(CaptureApplication*a,bool discard){
 if(!a)return false;
 if(a->state==CA_ACTIVE){
  bool ok=capture_runtime_request_stop(&a->runtime,discard);
  if(ok&&raw_current(a))lfn_raw_stop(a->raw,RAW_STOP_MANUAL);
  return ok;
 }
 if(a->state==CA_CLOCK){a->cancel_start=true;return true;}
 if(a->state==CA_PROBE||a->state==CA_BIND_DMA){a->state=CA_WAIT;return true;}
 if(a->state==CA_BOOT_PROOF){a->cancel_start=true;return true;}
 return false;
}
/* Exact cold-boot state: every ABI answers, CAP1 reports all-zero flags
 * (frontend idle, nothing running, no stop barrier yet), no CAP1 command
 * pending, DAT1 empty and its live status completely clear. Only this state
 * legitimately lacks the boot ownership proof. */
static bool boot_unproven(CaptureApplication*a){
 PlCaptureSnapshot s;PlDataAction action;
 if(pl_capture_probe(&a->cap)!=PL_OK||pl_data_probe(&a->dat)!=PL_OK||pl_contract_probe(&a->ctr)!=PL_OK)return false;
 if(pl_capture_snapshot(&a->cap,a->limits.runtime.snapshot_age_ticks,&s)!=PL_OK||s.flags!=0)return false;
 if(a->cap.read32(a->cap.context,0x14)&1)return false;
 if(pl_data_action(&a->dat,&action)!=PL_EMPTY)return false;
 return (a->dat.read32(a->dat.context,0x14)&7)==0;
}
static PlResult stopped(CaptureApplication*a){ PlResult p=pl_capture_probe(&a->cap);if(p==PL_OK)p=pl_data_probe(&a->dat);if(p==PL_OK)p=pl_contract_probe(&a->ctr);
 if(p!=PL_OK)return p;
 PlCaptureSnapshot s;p=pl_capture_snapshot(&a->cap,a->limits.runtime.snapshot_age_ticks,&s);if(p!=PL_OK)return p;
 PlDataAction action;p=pl_data_action(&a->dat,&action);
 if(p!=PL_EMPTY)return p==PL_OK?PL_REJECTED:p;
 uint32_t live=a->dat.read32(a->dat.context,0x14);
 if(!(s.flags&PL_CAPTURE_F_STOP_COMPLETE)||(s.flags&(PL_CAPTURE_F_RUNNING|PL_CAPTURE_F_CLOCK_BUSY|PL_CAPTURE_F_SERVICE_BUSY|PL_CAPTURE_F_QUARANTINED))||
    (a->cap.read32(a->cap.context,0x14)&1)||(live&7)!=4)return PL_REJECTED;
 return PL_OK;
}
static void step_runtime(CaptureApplication*a){
 if(!a||a->state==CA_WAIT||a->state==CA_FAULT)return;
 if(a->state==CA_ACTIVE){capture_runtime_step(&a->runtime);return;}
 uint64_t t=us(a);if(a->have_time&&t<a->last_now){a->failure=PL_TIMEOUT;a->cancel_start=true;}
 a->last_now=t;if(!a->have_time){a->since=t;a->have_time=true;}
 if(a->failure!=PL_OK&&a->state!=CA_CLOCK){fail(a,a->failure);return;}
 switch(a->state){
 case CA_PROBE:{
  PlResult p=stopped(a);
  if(p!=PL_OK){
   /* Cold boot: real RTL resets DAT1 dma_idle to 0, so the very first start
    * must perform the initial ownership DRAIN proof itself. Host register
    * models grant idle at reset; silicon does not. The attempt fires only
    * for the exact cold state (all ABIs valid, CAP1 flags all zero, no
    * pending command, DAT1 empty and not idle); any other rejection is a
    * real fault and fails immediately without side effects. */
   if(p==PL_REJECTED&&!a->boot_proof_done&&!a->boot_pending&&boot_unproven(a)){
    uint32_t seq=0;
    if(pl_data_request(&a->dat,PL_DATA_DRAIN,&seq)==PL_OK&&seq){
     a->boot_pending=true;a->boot_acked=false;a->boot_ticket=seq;a->since=t;a->state=CA_BOOT_PROOF;break;
    }
   }
   fail(a,p);break;
  }
  /* CAP1+8 bits [2:0] = restart pending, sticky loss, MCLK ready.
   * Restarting a healthy MMCM deliberately drops ready and latches loss. */
  uint32_t clock=a->cap.read32(a->cap.context,8);
  if(a->selected.source==PL_CAPTURE_ADC&&(clock&2u)){fail(a,PL_REJECTED);break;}
  a->clock_may_clear=(clock&7u)==1u;
  p=pl_capture_command_request(&a->cap,PL_CAPTURE_CLOCK_RESTART,&a->clock_ticket);
  if(p!=PL_OK){fail(a,p);break;}a->clock_pending=true;a->since=t;a->state=CA_CLOCK;break;
 }
 case CA_BOOT_PROOF:{
  if(t<a->since||t-a->since>=a->limits.runtime.transaction_us){fail(a,PL_TIMEOUT);break;}
  uint32_t status=0;PlResult p=pl_data_poll(&a->dat,a->boot_ticket,&status);
  if(p!=PL_OK&&p!=PL_BUSY){fail(a,p);break;}
  PlDataAction action;PlResult q=pl_data_action(&a->dat,&action);
  if(q!=PL_OK&&q!=PL_EMPTY){fail(a,q);break;}
  if(a->boot_acked){
   if(a->dat.read32(a->dat.context,0x2c)!=2){fail(a,PL_REJECTED);break;}
   if(a->dat.read32(a->dat.context,0x18)!=a->boot_action.seq||
      (q==PL_OK&&(action.seq!=a->boot_action.seq||action.mask!=3||action.reset||action.resume))){fail(a,PL_STALE);break;}
  }else if(q==PL_OK){
   if(action.mask!=3||action.reset||action.resume||!a->hooks.boot_dma_idle){fail(a,PL_REJECTED);break;}
   if(!a->hooks.boot_dma_idle(a->hooks.context))break;
   q=pl_data_ack(&a->dat,&action);if(q!=PL_OK){fail(a,q);break;}
   a->boot_action=action;a->boot_acked=true;break;
  }
  if(p==PL_BUSY)break; /* Retain exact ticket, including after cancellation. */
  if(!a->boot_acked||q!=PL_EMPTY){fail(a,PL_STALE);break;}
  a->boot_pending=false;a->boot_proof_done=true;a->state=a->cancel_start?CA_WAIT:CA_PROBE;break;
 }
 case CA_CLOCK:{
  if(t<a->since||t-a->since>=a->limits.runtime.transaction_us){a->failure=PL_TIMEOUT;a->cancel_start=true;}
  uint32_t status=0;PlResult p=pl_capture_poll(&a->cap,&a->clock_ticket,&status);
  if(p==PL_STALE||p==PL_BUS||p==PL_INVALID||(p==PL_REJECTED&&!status)){fail(a,p);break;}
  if(p==PL_TIMEOUT){a->failure=p;a->cancel_start=true;}
  if(status&PL_BUSY_BIT)break; /* Retain exact ticket, no START or DMA binding. */
  a->clock_pending=false;
  if(p!=PL_OK||a->failure!=PL_OK){fail(a,p==PL_OK?a->failure:p);break;}
  uint32_t clock=a->cap.read32(a->cap.context,8)&7u;
  if(a->clock_may_clear&&clock==3u){
   a->clock_may_clear=false;
   p=pl_capture_command_request(&a->cap,PL_CAPTURE_CLOCK_CLEAR,&a->clock_ticket);
   if(p!=PL_OK){fail(a,p);break;}
   a->clock_pending=true;a->since=t;break;
  }
  if(!a->cancel_start&&a->selected.source==PL_CAPTURE_ADC&&clock!=1u){fail(a,PL_REJECTED);break;}
  a->state=a->cancel_start?CA_WAIT:CA_BIND_DMA;break;
 }
 case CA_BIND_DMA:{
  PlResult p=stopped(a);if(p!=PL_OK){fail(a,p);break;}
  if(!a->hooks.bind_dma(a->hooks.context)){fail(a,PL_REJECTED);break;}
  CaptureDataHooks dh={a,halt,idle,rearm};CaptureRuntimeHooks rh={a,us,prepare,analysis,idle_analysis,fault};
  if(!capture_data_owner_init(&a->owner,&a->dat,a->flows[0],a->flows[1],&dh,a->limits.owner_us)||
     !capture_runtime_init(&a->runtime,&a->cap,&a->ctr,&a->task,&a->owner,&rh,&a->limits.runtime)||
     !capture_runtime_request_start(&a->runtime,a->session,&a->selected)){fail(a,PL_REJECTED);break;}
  a->bound=true;a->state=CA_ACTIVE;break;
 }
 default:fail(a,PL_INVALID);break;
 }
}

bool capture_application_attach_services(CaptureApplication*a,LfnServiceBridge*b){
 if(!a||!b||!b->initialized||a->services||a->state!=CA_WAIT||a->bound||a->prepared||a->clock_pending)return false;
 a->services=b;return true;
}
bool capture_application_attach_history(CaptureApplication*a,PcmHistory*h){
 if(!a||!h||a->history||a->state!=CA_WAIT||a->bound||a->prepared||a->clock_pending)return false;
 a->history=h;return true;
}
bool capture_application_attach_raw(CaptureApplication*a,LfnRawCapture*r){
 if(!a||!r||!r->buffer||a->raw||a->state!=CA_WAIT||a->bound||a->prepared||a->clock_pending)return false;
 a->raw=r;return true;
}
StorageResult capture_application_attach_storage_result(CaptureApplication*a,CaptureStorageRoute*r){
 if(!a||!r||!a->services||a->storage||a->clock_pending||a->storage_close_requested||a->storage_input_closed)return STORE_INVALID;
 bool fresh=a->state==CA_WAIT&&!a->bound&&!a->prepared;
 bool reattach=a->state==CA_ACTIVE&&a->runtime.state==CR_STOPPED&&a->bound&&a->prepared&&!a->plan_valid;
 if(!fresh&&!reattach)return STORE_INVALID;
 StorageResult result=capture_storage_route_bind_result(r,a->services);if(result!=STORE_OK)return result;
 a->storage=r;a->storage_generation=1;return STORE_OK;
}
bool capture_application_attach_storage(CaptureApplication*a,CaptureStorageRoute*r){return capture_application_attach_storage_result(a,r)==STORE_OK;}
bool capture_application_close_storage(CaptureApplication*a){
 if(!a||!a->storage||a->state!=CA_ACTIVE||a->runtime.state!=CR_STOPPED||!a->runtime.final_clean)return false;
 a->storage_close_requested=true;return true;
}
StorageResult capture_application_detach_storage(CaptureApplication*a){
 if(!a||!a->storage)return STORE_INVALID;
 if(a->state!=CA_ACTIVE||a->runtime.state!=CR_STOPPED||a->clock_pending)return STORE_INVALID;
 if(!a->storage_input_closed){
  /* Production runtime closes the route itself; accept detach when the route
   * is already closed and fully drained, otherwise keep the Host-test rule. */
  CaptureRouteHealth h;
  if(!capture_storage_route_health(a->storage,&h))return STORE_INVALID;
  if(!h.attached||!h.producer_closed||!h.consumer.handoff_drained||h.consumer.blocked)return STORE_INVALID;
 }
 StorageResult result=capture_storage_route_unbind(a->storage,a->storage_generation);
 if(result!=STORE_OK)return result;
 a->storage=NULL;a->storage_close_requested=false;a->storage_input_closed=false;
 a->storage_generation=0;a->plan_valid=false;a->accepted_capture=false;return STORE_OK;
}
bool capture_application_resume_storage(CaptureApplication*a,uint64_t generation){
 if(!a||!a->storage||!a->storage_input_closed||a->storage_close_requested||a->clock_pending||a->state!=CA_ACTIVE||a->runtime.state!=CR_STOPPED||a->storage_generation==UINT64_MAX||generation!=a->storage_generation+1)return false;
 CaptureRouteHealth h;if(!capture_storage_route_health(a->storage,&h)||!h.attached||h.producer_closed||h.consumer.handoff_drained||h.consumer.blocked||h.generation!=generation)return false;
 a->storage_generation=generation;a->storage_input_closed=false;a->accepted_capture=false;return true;
}
static void raw_stop_request(CaptureApplication*a){
 if(raw_current(a)&&a->raw->status.reason!=RAW_STOP_NONE&&a->state==CA_ACTIVE&&
    (a->runtime.state==CR_BIND||a->runtime.state==CR_RUN))
  (void)capture_runtime_request_stop(&a->runtime,false);
}
void capture_application_step(CaptureApplication*a){
 if(!a)return;
 if(raw_current(a)){lfn_raw_tick(a->raw,us(a));raw_stop_request(a);}
 step_runtime(a);
 if(a->state==CA_ACTIVE&&(a->runtime.state==CR_BIND||a->runtime.state==CR_RUN)){
  a->accepted_capture=true;if(raw_current(a))lfn_raw_start(a->raw,us(a));
 }
 if(raw_current(a)){
  if(a->task.missing[LFN_RAW-1])lfn_raw_stop(a->raw,RAW_STOP_INPUT_LOSS);
  raw_stop_request(a);
  if(a->state==CA_ACTIVE&&(a->runtime.state==CR_STOPPED||a->runtime.state==CR_FAILED||a->runtime.state==CR_QUARANTINED))
   lfn_raw_finish(a->raw,a->runtime.state!=CR_STOPPED);
 }
 if(!a->services)return;
 LfnServiceBridge*b=a->services;(void)lfn_service_flush(b);
 LfnServiceSnapshot*s=&b->staging;memset(s,0,sizeof(*s));
 if(a->raw)s->raw=a->raw->status;
 s->requested_session=a->session;s->requested=a->selected;
 s->application_state=(uint32_t)a->state;s->runtime_state=(uint32_t)a->runtime.state;
 s->failure=(int32_t)(a->failure?a->failure:a->runtime.failure);
 memcpy(s->packets,a->packets,sizeof(s->packets));s->energy_records=a->energy_records;s->energy_gaps=a->energy_gaps;
 s->prepared_valid=a->prepared&&a->plan_valid;s->capture_valid=a->accepted_capture&&a->plan_valid;
 if(s->prepared_valid){
  s->prepared=(LfnServiceIdentity){a->task.config.session_id,a->task.config.capture_id,a->active_config};
  if(a->accepted_capture)s->active=s->prepared;
  (void)lfn_analysis_snapshot_copy(&a->analysis,&s->analysis);
 }
 if(a->storage){
  CaptureRouteFacts facts={0};facts.observed_us=us(a);facts.generation=a->storage_generation;
  if(a->prepared&&a->plan_valid&&a->accepted_capture){
   facts.identity=s->prepared;facts.bound_mask=a->task.anchor_bound_mask;memcpy(facts.first_seq,a->task.config.first_seq,sizeof(facts.first_seq));memcpy(facts.first_raw,a->task.first_raw,sizeof(facts.first_raw));
   facts.software_drained=a->runtime.state==CR_STOPPED||a->runtime.state==CR_FAILED;
   const CaptureRuntime *rt=&a->runtime;
   facts.final_clean=rt->state==CR_STOPPED&&rt->final_clean&&rt->final_session==facts.identity.session_id&&
    rt->final_capture.capture_id==facts.identity.capture_id&&rt->final_capture.active_config_id==facts.identity.config.config_id;
   if(facts.final_clean){facts.final_latest_raw=rt->final_capture.latest_raw;facts.final_capture_seq=rt->final_capture.seq;facts.final_drain_seq=rt->final_drain_seq;}
   const CaptureTimePair *pair=&a->runtime.time_tracker.pair;
   if(pair->session_id==facts.identity.session_id&&pair->capture_id==facts.identity.capture_id&&
      pair->config_id==facts.identity.config.config_id&&pair->source==facts.identity.config.source)facts.time_pair=*pair;
   if(a->runtime.have_observed_started&&a->runtime.observed_started.capture_id==facts.identity.capture_id){facts.started_valid=true;facts.started=a->runtime.observed_started;}
   if(a->runtime.have_observed_capture&&a->runtime.observed_capture.capture_id==facts.identity.capture_id){facts.capture_valid=true;facts.capture=a->runtime.observed_capture;}
   if(a->runtime.have_observed_active&&a->runtime.observed_active.plan.capture_id==facts.identity.capture_id){facts.active_valid=true;facts.active=a->runtime.observed_active;}
  }
  bool published=capture_storage_route_facts_publish(a->storage,&facts);
  if(s->analysis.has_spectrum)(void)capture_storage_route_psd(a->storage,&s->prepared,&s->analysis.spectrum);
  capture_storage_route_flush(a->storage);
  if(a->storage_close_requested&&published&&facts.final_clean&&!b->pending_loss&&capture_storage_route_producer_close(a->storage)){a->storage_close_requested=false;a->storage_input_closed=true;}
 }
 (void)lfn_service_snapshot_publish(b,us(a));
}

