#include "pre_adc_controller.h"
#include <string.h>
static bool config_equal(const PlCaptureConfig*a,const PlCaptureConfig*b){
 return a->mode==b->mode&&a->source==b->source&&a->channel==b->channel&&a->pattern==b->pattern&&a->config_id==b->config_id;
}
static bool limits_equal(const CaptureApplicationLimits*a,const CaptureApplicationLimits*b){
 return a->runtime.transaction_us==b->runtime.transaction_us&&a->runtime.bind_us==b->runtime.bind_us&&
 a->runtime.close_us==b->runtime.close_us&&a->runtime.snapshot_age_ticks==b->runtime.snapshot_age_ticks&&
 a->owner_us==b->owner_us&&a->raw.duration_us==b->raw.duration_us&&a->raw.capacity_bytes==b->raw.capacity_bytes;
}
static bool command_equal(const PreAdcCommand*a,const PreAdcCommand*b){
 return a->id==b->id&&a->session_id==b->session_id&&a->kind==b->kind&&a->discard==b->discard&&
 config_equal(&a->config,&b->config)&&limits_equal(&a->limits,&b->limits);
}
static void emit(PreAdcController*c,uint64_t command,PreAdcOutcome result){
 const PreAdcStatus*s=&c->status;
 PreAdcDiagEvent e={command,s->session_id,s->capture_id,s->snapshot_version,s->config_id,
 (uint32_t)s->state,(uint32_t)result,(uint32_t)s->raw_reason,s->failure};
 (void)pre_adc_diag_offer(c->diag,&e);
}
static void fault_latch(PreAdcController*c,int32_t failure){
 c->status.fault_latched=true;c->status.state=PRE_ADC_FAULT;
 if(!c->status.failure)c->status.failure=failure?failure:(int32_t)PL_REJECTED;
 c->status.start=PRE_ADC_FAILED;
 if(c->status.stop_command)c->status.stop=PRE_ADC_FAILED;
}
static void copy_snapshot(PreAdcController*c){
 LfnServiceSnapshot s;
 if(c->hooks.snapshot_copy(c->hooks.context,&s)==SERVICE_OK&&s.version>c->status.snapshot_version){
  c->snapshot=s;c->status.snapshot_version=s.version;c->status.have_snapshot=true;
 }
}
bool pre_adc_controller_init(PreAdcController*c,uint64_t session,const PreAdcHooks*h,PreAdcDiag*d){
 if(!c||!session||!h||!h->submit_start||!h->submit_stop||!h->request_copy||!h->request_ack||!h->snapshot_copy||!d)return false;
 PreAdcHooks saved=*h;memset(c,0,sizeof(*c));c->hooks=saved;c->diag=d;c->initialized=true;c->status.session_id=session;return true;
}
static bool valid_start(const PreAdcCommand*q){
 const CaptureApplicationLimits*l=&q->limits;
 bool source_valid=q->kind==PRE_ADC_START_TEST?
  q->config.source==PL_CAPTURE_TEST&&q->config.pattern<=3:
  q->kind==PRE_ADC_START_ADC&&q->config.source==PL_CAPTURE_ADC&&q->config.pattern==0;
 if(q->discard||!source_valid||q->config.mode>2||q->config.channel>1||!q->config.config_id||
 !l->runtime.transaction_us||!l->runtime.bind_us||!l->runtime.close_us||!l->runtime.snapshot_age_ticks||!l->owner_us)return false;
 if(!q->config.mode)return !l->raw.duration_us&&!l->raw.capacity_bytes;
 return l->raw.duration_us&&l->raw.capacity_bytes>=LFN_RAW_PACKET_BYTES&&l->raw.capacity_bytes<=LFN_RAW_BUFFER_BYTES;
}
static void queue_start(PreAdcController*c,const PreAdcCommand*q){
 c->start_floor=c->status.snapshot_version;c->start_command=*q;c->max_config=q->config.config_id;
 c->pending_start=true;c->start_accepted=false;c->stop_accepted=false;
 c->status.state=PRE_ADC_STARTING;c->status.start=PRE_ADC_QUEUED;c->status.stop=PRE_ADC_NONE;
 c->status.start_command=q->id;c->status.stop_command=0;c->status.capture_id=0;c->status.config_id=q->config.config_id;
 c->status.raw_reason=RAW_STOP_NONE;
}
PreAdcResult pre_adc_controller_restart(PreAdcController*c,const PlCaptureConfig*config){
 if(!c||!c->initialized||!config)return PRE_ADC_INVALID;
 if(c->status.fault_latched||c->status.state!=PRE_ADC_RUNNING||c->ticket||c->pending_start||c->pending_stop||c->stop_accepted)return PRE_ADC_BUSY;
 PlCaptureConfig previous=*config;previous.config_id=c->start_command.config.config_id;
 if(config->config_id<=c->max_config||!config_equal(&previous,&c->start_command.config))return PRE_ADC_INVALID;
 copy_snapshot(c);
 const LfnServiceSnapshot*s=&c->snapshot;
 if(!c->status.have_snapshot||s->version<=c->start_floor||s->requested_session!=c->status.session_id||
    !config_equal(&s->requested,&c->start_command.config)||s->application_state!=CA_ACTIVE||s->runtime_state!=CR_STOPPED||s->failure!=PL_OK)return PRE_ADC_BUSY;
 /* A rotation continues the original host START under a new capture identity. */
 PreAdcCommand q=c->start_command;q.config=*config;queue_start(c,&q);
 emit(c,q.id,c->status.start);return PRE_ADC_OK;
}
PreAdcResult pre_adc_controller_command(PreAdcController*c,const PreAdcCommand*q){
 if(!c||!c->initialized||!q||!q->id||q->session_id!=c->status.session_id)return PRE_ADC_INVALID;
 if(c->have_last&&q->id==c->last.id)return command_equal(q,&c->last)?PRE_ADC_DUPLICATE:PRE_ADC_INVALID;
 if(c->have_last&&q->id<c->last.id)return PRE_ADC_STALE;
 if(q->kind==PRE_ADC_RECOVER)return PRE_ADC_UNSUPPORTED;
 if(q->kind==PRE_ADC_START_TEST||q->kind==PRE_ADC_START_ADC){
  if(!valid_start(q)||q->config.config_id<=c->max_config)return PRE_ADC_INVALID;
  if(c->status.fault_latched||c->ticket||c->pending_start||c->pending_stop||
     (c->status.state!=PRE_ADC_IDLE&&c->status.state!=PRE_ADC_STOPPED))return PRE_ADC_BUSY;
  copy_snapshot(c);queue_start(c,q);
 }else if(q->kind==PRE_ADC_STOP){
  PlCaptureConfig zc={0};CaptureApplicationLimits zl={0};
  if(!config_equal(&q->config,&zc)||!limits_equal(&q->limits,&zl))return PRE_ADC_INVALID;
  if(c->pending_stop||c->stop_accepted||(c->ticket&&!c->request_is_start))return PRE_ADC_BUSY;
  if(!c->status.start_command||c->status.state==PRE_ADC_IDLE||c->status.state==PRE_ADC_STOPPED)return PRE_ADC_BUSY;
  c->stop_command=*q;c->pending_stop=true;c->status.stop_command=q->id;c->status.stop=PRE_ADC_QUEUED;
  if(!c->status.fault_latched)c->status.state=PRE_ADC_STOPPING;
 }else if(q->kind==PRE_ADC_STATUS){
  PlCaptureConfig zc={0};CaptureApplicationLimits zl={0};
  if(q->discard||!config_equal(&q->config,&zc)||!limits_equal(&q->limits,&zl))return PRE_ADC_INVALID;
 }else return PRE_ADC_INVALID;
 c->last=*q;c->have_last=true;emit(c,q->id,q->kind==PRE_ADC_STOP?c->status.stop:c->status.start);return PRE_ADC_OK;
}
static void request_step(PreAdcController*c){
 if(c->ticket){
  CaptureRequestReceipt r;
  if(c->hooks.request_copy(c->hooks.context,c->ticket,&r)!=STORE_OK)return;
  CaptureRequestKind expected=c->request_is_start?REQUEST_START:(c->stop_command.discard?REQUEST_DISCARD:REQUEST_STOP);
  if(r.ticket!=c->ticket||r.kind!=expected||r.session!=c->status.session_id||r.config_id!=c->status.config_id||r.source!=c->start_command.config.source)return;
  /* Retain exact ticket until ACK succeeds; never replace with STOP meanwhile. */
  if(c->hooks.request_ack(c->hooks.context,c->ticket)!=STORE_OK)return;
  c->ticket=0;
  if(c->request_is_start){
   c->status.start=r.result==STORE_OK?PRE_ADC_ACCEPTED:PRE_ADC_REJECTED;c->start_accepted=r.result==STORE_OK;
   if(!c->start_accepted){
    if(c->pending_stop){c->pending_stop=false;c->status.stop=PRE_ADC_REJECTED;}
    if(!c->status.fault_latched)c->status.state=PRE_ADC_STOPPED;
   }
  }else{c->status.stop=r.result==STORE_OK?PRE_ADC_ACCEPTED:PRE_ADC_REJECTED;c->stop_accepted=r.result==STORE_OK;}
  if(r.failure!=PL_OK)fault_latch(c,r.failure);
 }
 if(c->ticket)return;
 if(c->pending_start){
  StorageResult r=c->hooks.submit_start(c->hooks.context,c->status.session_id,&c->start_command.config,&c->start_command.limits,&c->ticket);
  if(r==STORE_BUSY)return;
  c->pending_start=false;c->request_is_start=true;
  if(r!=STORE_OK||!c->ticket){c->ticket=0;c->status.start=PRE_ADC_REJECTED;c->pending_stop=false;c->status.stop=c->status.stop_command?PRE_ADC_REJECTED:PRE_ADC_NONE;c->status.state=PRE_ADC_STOPPED;}
 }else if(c->pending_stop&&c->start_accepted){
  c->stop_floor=c->status.snapshot_version;
  StorageResult r=c->hooks.submit_stop(c->hooks.context,c->stop_command.discard,&c->ticket);
  if(r==STORE_BUSY)return;
  c->pending_stop=false;c->request_is_start=false;
  if(r!=STORE_OK||!c->ticket){c->ticket=0;c->status.stop=PRE_ADC_REJECTED;}
 }
}
static bool identity(const PreAdcController*c,const LfnServiceIdentity*i){
 return i->session_id==c->status.session_id&&i->capture_id&&config_equal(&i->config,&c->start_command.config);
}
static void observe(PreAdcController*c){
 const LfnServiceSnapshot*s=&c->snapshot;
 if(!c->start_accepted||!c->status.have_snapshot||s->version<=c->start_floor||
    s->requested_session!=c->status.session_id||!config_equal(&s->requested,&c->start_command.config))return;
 if(s->prepared_valid&&identity(c,&s->prepared))c->status.capture_id=s->prepared.capture_id;
 bool raw=c->start_command.config.mode&&c->status.capture_id&&s->raw.prepared&&s->raw.session_id==c->status.session_id&&
  s->raw.capture_id==c->status.capture_id&&s->raw.config_id==c->status.config_id;
 if(raw)c->status.raw_reason=s->raw.reason;
 if(s->failure!=PL_OK||s->application_state==CA_FAULT||s->runtime_state==CR_FAILED||s->runtime_state==CR_INCOMPATIBLE||s->runtime_state==CR_QUARANTINED||
    (raw&&(s->raw.cleanup_failed||s->raw.reason>=RAW_STOP_INPUT_LOSS))){fault_latch(c,s->failure);return;}
 if(c->status.fault_latched)return;
 bool stopped=s->application_state==CA_WAIT||(s->application_state==CA_ACTIVE&&s->runtime_state==CR_STOPPED);
 if(c->stop_accepted&&s->version>c->stop_floor&&stopped){
  c->status.state=PRE_ADC_STOPPED;c->status.stop=PRE_ADC_STOP_CONFIRMED;
  if(c->status.start==PRE_ADC_ACCEPTED)c->status.start=PRE_ADC_CANCELLED;
  return;
 }
 if(raw&&s->raw.started&&s->raw.terminal&&stopped&&(s->raw.reason==RAW_STOP_CAPACITY||s->raw.reason==RAW_STOP_TIME)){
  c->status.state=PRE_ADC_STOPPED;c->status.start=PRE_ADC_RAW_COMPLETE;return;
 }
 if(c->pending_stop||c->stop_accepted||(c->ticket&&!c->request_is_start)){c->status.state=PRE_ADC_STOPPING;return;}
 /* capture_valid first becomes true in CR_BIND. RUN additionally needs CR_RUN. */
 if(s->application_state==CA_ACTIVE&&s->runtime_state==CR_RUN&&s->capture_valid&&identity(c,&s->active)&&s->active.capture_id==c->status.capture_id){
  c->status.state=PRE_ADC_RUNNING;c->status.start=PRE_ADC_RUN_CONFIRMED;
 }
}
void pre_adc_controller_step(PreAdcController*c){
 if(!c||!c->initialized)return;
 PreAdcStatus before=c->status;copy_snapshot(c);request_step(c);observe(c);
 if(before.state!=c->status.state||before.start!=c->status.start||before.stop!=c->status.stop||before.capture_id!=c->status.capture_id||before.failure!=c->status.failure)
  emit(c,c->status.stop_command?c->status.stop_command:c->status.start_command,c->status.stop_command?c->status.stop:c->status.start);
}
bool pre_adc_controller_status(const PreAdcController*c,PreAdcStatus*s){
 if(!c||!c->initialized||!s)return false;
 *s=c->status;return true;
}
bool pre_adc_controller_can_rebegin(const PreAdcController*c,uint64_t session){
 if(!c||!c->initialized||!session)return false;
 if(session==c->status.session_id)return false;
 if(c->status.state!=PRE_ADC_STOPPED||c->status.stop!=PRE_ADC_STOP_CONFIRMED)return false;
 if(c->status.fault_latched||c->status.start==PRE_ADC_FAILED)return false;
 if(c->ticket||c->pending_start||c->pending_stop)return false;
 return true;
}
bool pre_adc_controller_rebegin(PreAdcController*c,uint64_t session){
 if(!pre_adc_controller_can_rebegin(c,session))return false;
 uint64_t snap=c->status.snapshot_version;
 PreAdcHooks hooks=c->hooks;PreAdcDiag*diag=c->diag;
 memset(c,0,sizeof(*c));
 c->hooks=hooks;c->diag=diag;c->initialized=true;
 c->status.session_id=session;c->status.state=PRE_ADC_IDLE;c->status.snapshot_version=snap;
 return true;
}
