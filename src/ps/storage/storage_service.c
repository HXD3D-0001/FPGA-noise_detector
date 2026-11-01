#include "storage_service.h"
#include <string.h>
#include <stdio.h>
static StorageSubmit reject(StorageResult r){StorageSubmit s={0};s.result=r;return s;}
static bool same_ticket(StorageTicket a,StorageTicket b){return a.session_id==b.session_id&&a.slot==b.slot&&a.generation==b.generation;}
static uint64_t satadd(uint64_t a,uint64_t b){return a>UINT64_MAX-b?UINT64_MAX:a+b;}
static void missing(StorageService*s,const RecordIdentity*i,uint64_t seq){
 if(!seq||seq==UINT64_MAX)return;
 bool known=(i->type==RECORD_ENERGY||i->type==RECORD_PCM||i->type==RECORD_PSD)&&i->first_raw<i->end_raw;
 StorageUnsavedRange r={i->session_id,i->capture_id,i->first_raw,i->end_raw,seq,seq+1,i->config_id,known};
 (void)storage_unsaved_add(&s->unsaved,&r); /* overflow is explicit, never erased */
}
static StorageServiceSlot*free_slot(StorageService*s,bool control){
 for(unsigned k=control?0:STORAGE_CONTROL_JOBS;k<STORAGE_RECEIPTS;k++)if(!s->slots[k].occupied)return &s->slots[k];
 return NULL;
}
static StorageServiceSlot*definition(StorageService*s,RecordType type,uint32_t config,uint64_t capture,uint64_t object){
 StorageServiceSlot*best=NULL;
 for(unsigned k=0;k<STORAGE_RECEIPTS;k++){
  StorageServiceSlot*x=&s->slots[k];RecordIdentity*i=&x->identity;
  if(!x->occupied||!x->definition||x->state==STORE_FAILED||i->type!=type)continue;
  if(config&&i->config_id!=config)continue;
  if(capture&&i->capture_id!=capture)continue;
  if(object&&x->object_id!=object)continue;
  if(!best||i->record_seq>best->identity.record_seq)best=x;
 }return best;
}
static bool dep(StorageRequest*r,StorageServiceSlot*x){
 if(!x||r->dependency_count==STORAGE_DEPENDENCIES)return false;
 for(unsigned k=0;k<r->dependency_count;k++)if(same_ticket(r->depends_on[k],x->ticket))return true;
 r->depends_on[r->dependency_count++]=x->ticket;return true;
}
static bool base(StorageService*s,StorageRequest*r){
 RecordIdentity*i=&r->identity;
 if(i->type==RECORD_SESSION)return true;
 if(!dep(r,definition(s,RECORD_SESSION,0,0,0)))return false;
 if(i->config_id&&!dep(r,definition(s,RECORD_CONFIG,i->config_id,0,0)))return false;
 if(i->capture_id&&!dep(r,definition(s,RECORD_CAPTURE,i->config_id,i->capture_id,0)))return false;
 return true;
}
static bool allowed(StorageService*s){
 return s&&s->queue&&s->registry&&!s->health.sealed&&!s->health.fatal&&s->queue->writer->state!=SW_FAULT;
}
bool storage_service_init(StorageService*s,StorageQueue*q,StorageMetadataRegistry*r){
 StorageQueueHealth h;
 if(!s||!q||!r||q->validate!=storage_registry_validate||q->validate_context!=r||q->writer->state!=SW_READY||
 atomic_load_explicit(&r->published,memory_order_acquire)||storage_queue_health(q,&h)!=STORE_OK||h.live_jobs||h.retained_receipts||h.accepted||q->writer->prefix)return false;
 memset(s,0,sizeof(*s));s->queue=q;s->registry=r;s->next_fault_id=1;return true;
}
static StorageSubmit submit_reserved(StorageService*s,StorageRequest*r,bool is_definition,const StorageMetadata*m,bool audit,bool reserve){
 StorageServiceSlot*x=free_slot(s,is_definition||audit||r->identity.type==RECORD_FAULT);
 if(!x)return reject(STORE_BUSY); /* no sequence allocation; caller retains input */
 StorageQueueRequest request={*r,true};StorageSubmit result=storage_queue_submit_reserved(s->queue,&request,reserve);
 if(result.result!=STORE_OK){
  if(result.sequence_assigned){missing(s,&r->identity,result.record_seq);s->health.capacity_rejected=satadd(s->health.capacity_rejected,1);}
  return result;
 }
 memset(x,0,sizeof(*x));x->occupied=true;x->definition=is_definition;x->audit=audit;x->state=STORE_QUEUED;
 x->ticket=result.ticket;x->identity=r->identity;x->identity.record_seq=result.record_seq;
 if(m){x->object_id=m->object_id;x->calibration_id=m->calibration_id;}
 if(s->observer.admission&&!s->observer.admission(s->observer.context,x,r)){x->observer_failed=true;s->health.fatal=true;}
 s->health.accepted=satadd(s->health.accepted,1);s->health.drained=false;return result;
}
static StorageSubmit submit(StorageService*s,StorageRequest*r,bool is_definition,const StorageMetadata*m,bool audit){return submit_reserved(s,r,is_definition,m,audit,false);}
StorageSubmit storage_service_metadata_reserved(StorageService*s,const RecordIdentity*i,const uint8_t*b,size_t n,bool reserve){
 if(!allowed(s))return reject(STORE_BUSY);
 if(!i||i->record_seq||i->session_id!=s->queue->session_id||i->type==RECORD_FAULT)return reject(STORE_INVALID);
 StorageQueueHealth h;if(storage_queue_health(s->queue,&h)!=STORE_OK||!h.ready)return reject(STORE_BUSY);
 StorageMetadata m;RecordIdentity reserved=*i;reserved.record_seq=h.next_seq;
 StorageMetaResult check=storage_registry_preflight(s->registry,&reserved,b,n,&s->scratch);
 if(check!=META_OK)return reject(check==META_CAPACITY?STORE_CAPACITY:check==META_REFERENCE?STORE_DEPENDENCY:STORE_INVALID);
 if(!storage_metadata_decode(i,b,n,&s->scratch,&m))return reject(STORE_INVALID);
 StorageRequest r={0};r.identity=*i;r.payload=b;r.payload_bytes=n;
 if(i->type!=RECORD_SESSION&&!dep(&r,definition(s,RECORD_SESSION,0,0,0)))return reject(STORE_DEPENDENCY);
 if(i->type!=RECORD_SESSION&&i->type!=RECORD_CONFIG&&!dep(&r,definition(s,RECORD_CONFIG,i->config_id,0,0)))return reject(STORE_DEPENDENCY);
 if(i->type==RECORD_TIME_MAP&&!dep(&r,definition(s,RECORD_CAPTURE,i->config_id,i->capture_id,0)))return reject(STORE_DEPENDENCY);
 if(i->type==RECORD_CAPTURE){StorageServiceSlot*old=definition(s,RECORD_CAPTURE,i->config_id,i->capture_id,0);if(old&&!dep(&r,old))return reject(STORE_DEPENDENCY);}
 if(i->type==RECORD_CALIBRATION&&m.calibrated&&!dep(&r,definition(s,RECORD_TIME_MAP,0,0,m.calibration_id)))return reject(STORE_DEPENDENCY);
 StorageSubmit result=submit_reserved(s,&r,true,&m,false,reserve);
 if(result.result==STORE_OK&&(result.record_seq!=reserved.record_seq||storage_registry_apply(s->registry,&(RecordIdentity){i->type,i->flags,i->config_id,i->session_id,i->capture_id,i->first_raw,i->end_raw,result.record_seq},b,n,&s->scratch)!=META_OK))s->health.fatal=true;
 return result;
}
StorageSubmit storage_service_metadata(StorageService*s,const RecordIdentity*i,const uint8_t*b,size_t n){
 if(!allowed(s))return reject(STORE_BUSY);
 if(!i||i->record_seq||i->session_id!=s->queue->session_id||i->type==RECORD_FAULT)return reject(STORE_INVALID);
 StorageQueueHealth h;if(storage_queue_health(s->queue,&h)!=STORE_OK||!h.ready)return reject(STORE_BUSY);
 StorageMetadata m;RecordIdentity reserved=*i;reserved.record_seq=h.next_seq;
 StorageMetaResult check=storage_registry_preflight(s->registry,&reserved,b,n,&s->scratch);
 if(check!=META_OK)return reject(check==META_CAPACITY?STORE_CAPACITY:check==META_REFERENCE?STORE_DEPENDENCY:STORE_INVALID);
 if(!storage_metadata_decode(i,b,n,&s->scratch,&m))return reject(STORE_INVALID);
 StorageRequest r={0};r.identity=*i;r.payload=b;r.payload_bytes=n;
 /* CONFIG and CAPTURE define themselves; only preceding definitions are dependencies. */
 if(i->type!=RECORD_SESSION&&!dep(&r,definition(s,RECORD_SESSION,0,0,0)))return reject(STORE_DEPENDENCY);
 if(i->type!=RECORD_SESSION&&i->type!=RECORD_CONFIG&&!dep(&r,definition(s,RECORD_CONFIG,i->config_id,0,0)))return reject(STORE_DEPENDENCY);
 if(i->type==RECORD_TIME_MAP&&!dep(&r,definition(s,RECORD_CAPTURE,i->config_id,i->capture_id,0)))return reject(STORE_DEPENDENCY);
 if(i->type==RECORD_CAPTURE){StorageServiceSlot*old=definition(s,RECORD_CAPTURE,i->config_id,i->capture_id,0);if(old&&!dep(&r,old))return reject(STORE_DEPENDENCY);}
 if(i->type==RECORD_CALIBRATION&&m.calibrated&&!dep(&r,definition(s,RECORD_TIME_MAP,0,0,m.calibration_id)))return reject(STORE_DEPENDENCY);
 StorageSubmit result=submit(s,&r,true,&m,false);
 if(result.result==STORE_OK&&(result.record_seq!=reserved.record_seq||storage_registry_apply(s->registry,&(RecordIdentity){i->type,i->flags,i->config_id,i->session_id,i->capture_id,i->first_raw,i->end_raw,result.record_seq},b,n,&s->scratch)!=META_OK))s->health.fatal=true;
 return result; /* fatal latches a broken ownership/publication invariant */
}
static bool notice(StorageService*s,const LfnServiceMessage*m,StorageRequest*r){
 bool delivery=m->kind==SERVICE_DELIVERY_LOSS;const CaptureEnergyMessage*e=&m->energy;
 if(!m->first_delivery||m->first_delivery>=m->end_delivery||(delivery?(!m->loss_reason||(m->loss_reason&~(LFN_DELIVERY_QUEUE_FULL|LFN_DELIVERY_LOCK_BUSY))):(!e->gap||!e->reason||(e->reason&~(ENERGY_GAP_SOURCE|ENERGY_GAP_POOL))||e->first_missing_seq>=e->end_missing_seq)))return false;
 if(!delivery){
  StorageServiceSlot*cfg=definition(s,RECORD_CONFIG,m->identity.config.config_id,0,0);
  if(!cfg||m->identity.config.mode||e->record.capture_id!=m->identity.capture_id||e->record.config_id!=m->identity.config.config_id)return false;
  const StorageMetadata*actual=NULL;unsigned used=atomic_load_explicit(&s->registry->published,memory_order_acquire);
  for(unsigned k=0;k<used;k++){const StorageMetadata*x=&s->registry->entries[k].value;if(x->identity.type==RECORD_CONFIG&&x->identity.config_id==m->identity.config.config_id)actual=x;}
  if(!actual||actual->config.source!=m->identity.config.source||actual->config.channel!=m->identity.config.channel||actual->config.pattern!=m->identity.config.pattern)return false;
 }
 StorageServiceSlot*session=definition(s,RECORD_SESSION,0,0,0);if(!session||s->next_fault_id==UINT64_MAX)return false;
 const StorageMetadataEntry*entry=NULL;unsigned count=atomic_load_explicit(&s->registry->published,memory_order_acquire);
 for(unsigned k=0;k<count;k++)if(s->registry->entries[k].value.identity.type==RECORD_SESSION){entry=&s->registry->entries[k];break;}
 if(!entry)return false;
 uint64_t cap=delivery?0:m->identity.capture_id;uint32_t cfg=delivery?0:m->identity.config.config_id;
 if(!delivery&&(m->identity.session_id!=s->queue->session_id||!cap||!cfg))return false;
 char capture[24],config[16];if(delivery){memcpy(capture,"null",5);memcpy(config,"null",5);}else{(void)snprintf(capture,sizeof(capture),"\"%llu\"",(unsigned long long)cap);(void)snprintf(config,sizeof(config),"%u",(unsigned)cfg);}
 uint64_t first=delivery?m->first_delivery:e->first_missing_seq,end=delivery?m->end_delivery:e->end_missing_seq;
 int n=snprintf((char*)s->payload,sizeof(s->payload),"{\"schema_version\":1,\"device_id\":\"%s\",\"session_id\":\"%llu\",\"capture_id\":%s,\"config_id\":%s,\"fault_id\":\"%llu\",\"component\":\"service\",\"reason\":\"%s\",\"stream_mask\":%u,\"epoch\":\"0\",\"count_delta\":\"%llu\",\"monotonic_start_ns\":\"0\",\"monotonic_end_ns\":\"0\",\"ranges_exact\":false,\"overflow_count\":\"0\",\"sample_ranges\":null,\"record_seq_ranges\":[],\"monotonic_known\":false,\"notice_range_kind\":\"%s\",\"notice_first\":\"%llu\",\"notice_end\":\"%llu\",\"notice_reason\":%u}",entry->value.device_id,(unsigned long long)s->queue->session_id,capture,config,(unsigned long long)s->next_fault_id,delivery?"DELIVERY_LOSS":"SOURCE_GAP",delivery?0u:8u,(unsigned long long)(end-first),delivery?"DELIVERY_ORDINAL":"ENERGY_CANDIDATE",(unsigned long long)first,(unsigned long long)end,(unsigned)(delivery?m->loss_reason:e->reason));
 if(n<0||(size_t)n>=sizeof(s->payload))return false;
 memset(r,0,sizeof(*r));r->identity=(RecordIdentity){RECORD_FAULT,0,cfg,s->queue->session_id,cap,0,0,0};r->payload=s->payload;r->payload_bytes=(size_t)n;return base(s,r);
}
static bool data_dependencies(StorageService*s,StorageRequest*r){
 if(!base(s,r))return false;
 StorageServiceSlot*cfg=definition(s,RECORD_CONFIG,r->identity.config_id,0,0);
 return !cfg||!cfg->calibration_id||dep(r,definition(s,RECORD_CALIBRATION,r->identity.config_id,0,cfg->calibration_id));
}
StorageSubmit storage_service_psd(StorageService*s,const LfnSpectrum*spectrum){
 if(!allowed(s))return reject(STORE_BUSY);
 if(!spectrum)return reject(STORE_INVALID);
 StoragePolicyCopy p;
 if(spectrum->session_id!=s->queue->session_id||storage_registry_policy(s->registry,spectrum->session_id,spectrum->capture_id,spectrum->config_id,0,&p)!=META_OK)return reject(STORE_DEPENDENCY);
 StorageRequest r;
 if(!storage_psd_record(&p.policy,spectrum,&r,s->payload,sizeof(s->payload)))return reject(STORE_INVALID);
 if(!data_dependencies(s,&r))return reject(STORE_DEPENDENCY);
 return submit(s,&r,false,NULL,false);
}
StorageResult storage_service_stage_pcm(StorageService*s,uint64_t session,const uint8_t*wire,size_t n){
 if(!allowed(s)||s->pcm_pending)return STORE_BUSY;
 PacketView v;if(session!=s->queue->session_id||!wire||n>sizeof(s->pcm_packet)||!packet_decode(wire,n,&v)||(v.header.type!=LFN_PCM&&v.header.type!=LFN_RAW))return STORE_INVALID;
 StoragePolicyCopy p;if(storage_registry_policy(s->registry,session,v.header.capture_id,v.header.config_id,0,&p)!=META_OK)return STORE_DEPENDENCY;
 /* Validate the entire packet, including the second RAW piece, before staging.
  * No sequence allocation and no partial acceptance on malformed input. */
 for(size_t offset=0;offset<v.value_count;offset+=4096){
  size_t count=v.value_count-offset;if(count>4096)count=4096;StorageRequest r;
  if(!storage_pcm_record(&p.policy,&v,offset,count,&r,s->payload,sizeof(s->payload))||!storage_registry_validate(s->registry,&r.identity,r.payload,r.payload_bytes))return STORE_INVALID;
  if(!data_dependencies(s,&r))return STORE_DEPENDENCY;
 }
 memcpy(s->pcm_packet,wire,n);s->pcm_bytes=n;s->pcm_next=0;s->pcm_session=session;s->pcm_pending=true;s->health.drained=false;return STORE_OK;
}
StorageSubmit storage_service_pcm_next(StorageService*s){
 if(!allowed(s))return reject(STORE_BUSY);
 if(!s->pcm_pending)return reject(STORE_EXPIRED);
 PacketView v;if(!packet_decode(s->pcm_packet,s->pcm_bytes,&v))return reject(STORE_INVALID);
 StoragePolicyCopy p;if(storage_registry_policy(s->registry,s->pcm_session,v.header.capture_id,v.header.config_id,0,&p)!=META_OK)return reject(STORE_DEPENDENCY);
 size_t count=v.value_count-s->pcm_next;if(count>4096)count=4096;StorageRequest r;
 if(!storage_pcm_record(&p.policy,&v,s->pcm_next,count,&r,s->payload,sizeof(s->payload)))return reject(STORE_INVALID);
 if(!data_dependencies(s,&r))return reject(STORE_DEPENDENCY);
 StorageSubmit result=submit(s,&r,false,NULL,false);
 if(result.result==STORE_OK){s->pcm_next+=count;if(s->pcm_next==v.value_count)s->pcm_pending=false;}
 return result;
}
bool storage_service_discard_pcm(StorageService*s,StoragePcmRemainder*out){
 if(!s||!out||!s->pcm_pending)return false;
 PacketView v;if(!packet_decode(s->pcm_packet,s->pcm_bytes,&v)||s->pcm_next>=v.value_count)return false;
 *out=(StoragePcmRemainder){s->pcm_session,v.header.capture_id,v.header.first_raw_sample_index+s->pcm_next*v.raw_ticks_per_sample,v.end_raw,v.header.config_id,s->pcm_next,v.value_count-s->pcm_next};s->pcm_pending=false;return true;
}
StorageSubmit storage_service_ingress_loss(StorageService*s,const StorageIngressLoss*m){
 if(!allowed(s))return reject(STORE_BUSY);
 if(!m||!m->count||!m->reason||(m->reason&~7u)||m->identity.session_id!=s->queue->session_id||m->stream>3||s->next_fault_id==UINT64_MAX)return reject(STORE_INVALID);
 StorageServiceSlot*session=definition(s,RECORD_SESSION,0,0,0);if(!session)return reject(STORE_DEPENDENCY);
 const char*device=NULL;unsigned count=atomic_load_explicit(&s->registry->published,memory_order_acquire);
 for(unsigned k=0;k<count;k++)if(s->registry->entries[k].value.identity.type==RECORD_SESSION){device=s->registry->entries[k].value.device_id;break;}
 if(!device)return reject(STORE_DEPENDENCY);
 bool known=m->stream!=0;char capture[24],config[16],samples[96];
 if(known){StoragePolicyCopy policy;if(m->first_raw>=m->end_raw||storage_registry_policy(s->registry,m->identity.session_id,m->identity.capture_id,m->identity.config.config_id,0,&policy)!=META_OK)return reject(STORE_DEPENDENCY);
  const PlCaptureConfig*a=&m->identity.config,*b=&policy.policy.config;
  if(a->mode!=b->mode||a->source!=b->source||a->channel!=b->channel||a->pattern!=b->pattern||(m->stream==3)!=(a->mode!=0))return reject(STORE_INVALID);
  (void)snprintf(capture,sizeof(capture),"\"%llu\"",(unsigned long long)m->identity.capture_id);(void)snprintf(config,sizeof(config),"%u",(unsigned)m->identity.config.config_id);(void)snprintf(samples,sizeof(samples),"[[\"%llu\",\"%llu\"]]",(unsigned long long)m->first_raw,(unsigned long long)m->end_raw);
 }else{if(m->identity.capture_id||m->identity.config.config_id||m->reason!=STORAGE_INGRESS_OVERFLOW)return reject(STORE_INVALID);memcpy(capture,"null",5);memcpy(config,"null",5);memcpy(samples,"null",5);}
 int n=snprintf((char*)s->payload,sizeof(s->payload),"{\"schema_version\":1,\"device_id\":\"%s\",\"session_id\":\"%llu\",\"capture_id\":%s,\"config_id\":%s,\"fault_id\":\"%llu\",\"component\":\"capture_route\",\"reason\":\"%s\",\"stream_mask\":%u,\"epoch\":\"0\",\"count_delta\":\"%llu\",\"monotonic_start_ns\":\"0\",\"monotonic_end_ns\":\"0\",\"monotonic_known\":false,\"ranges_exact\":%s,\"overflow_count\":\"%llu\",\"sample_ranges\":%s,\"record_seq_ranges\":[],\"candidate_seq\":\"%llu\",\"route_reason\":%u}",device,(unsigned long long)m->identity.session_id,capture,config,(unsigned long long)s->next_fault_id,known?"INGRESS_LOSS":"INGRESS_OVERFLOW",known?1u<<(m->stream-1):0u,(unsigned long long)m->count,known?"true":"false",(unsigned long long)(known?0:m->count),samples,(unsigned long long)(known?m->stream_seq:0),(unsigned)m->reason);
 if(n<0||(size_t)n>=sizeof(s->payload))return reject(STORE_INVALID);
 StorageRequest r={0};r.identity=(RecordIdentity){RECORD_FAULT,0,known?m->identity.config.config_id:0,m->identity.session_id,known?m->identity.capture_id:0,known?m->first_raw:0,known?m->end_raw:0,0};r.payload=s->payload;r.payload_bytes=(size_t)n;if(!base(s,&r))return reject(STORE_DEPENDENCY);
 StorageSubmit result=submit(s,&r,false,NULL,false);if(result.result==STORE_OK)s->next_fault_id++;return result;
}
StorageSubmit storage_service_offer(StorageService*s,const LfnServiceMessage*m){
 if(!allowed(s))return reject(STORE_BUSY);
 if(!m)return reject(STORE_INVALID);
 StorageRequest r={0};bool loss=m->kind==SERVICE_DELIVERY_LOSS||(m->kind==SERVICE_ENERGY&&m->energy.gap);
 if(loss){if(!notice(s,m,&r))return reject(STORE_DEPENDENCY);}
 else{
  StoragePolicyCopy p;if(m->kind!=SERVICE_ENERGY||storage_registry_policy(s->registry,m->identity.session_id,m->identity.capture_id,m->identity.config.config_id,0,&p)!=META_OK)return reject(STORE_DEPENDENCY);
  if(!storage_energy_record(&p.policy,m,&r,s->payload,sizeof(s->payload))){s->health.invalid=satadd(s->health.invalid,1);return reject(STORE_INVALID);}
  if(!data_dependencies(s,&r))return reject(STORE_DEPENDENCY);
 }
 StorageSubmit result=submit(s,&r,false,NULL,false);
 if(result.result==STORE_OK&&loss){s->next_fault_id++;if(m->kind==SERVICE_DELIVERY_LOSS)s->health.delivery_notices=satadd(s->health.delivery_notices,1);else s->health.source_notices=satadd(s->health.source_notices,1);}
 return result;
}
StorageResult storage_service_take(StorageService*s,LfnServiceBridge*b){
 if(!allowed(s)||!b)return STORE_BUSY;
 if(!s->health.pending){LfnServiceResult r=lfn_service_energy_take(b,&s->pending);if(r==SERVICE_EMPTY)return STORE_EXPIRED;if(r!=SERVICE_OK)return r==SERVICE_BUSY?STORE_BUSY:STORE_INVALID;s->health.pending=true;}
 StorageSubmit result=storage_service_offer(s,&s->pending);s->health.pending_result=result.result;
 if(result.result==STORE_OK)s->health.pending=false;
 return result.result;
}
bool storage_service_discard_pending(StorageService*s,LfnServiceMessage*out){
 if(!s||!out||!s->health.pending)return false;
 *out=s->pending;s->health.pending=false;s->health.invalid=satadd(s->health.invalid,1);return true;
}
bool storage_service_seal(StorageService*s){if(!s||!s->queue||s->health.pending||s->pcm_pending||s->health.fatal)return false;s->health.sealed=true;return true;}
static void merge_back(StorageService*s){
 for(unsigned k=0;k<s->inflight.count;k++)(void)storage_unsaved_add(&s->unsaved,&s->inflight.ranges[k]);
 s->unsaved.overflow=satadd(s->unsaved.overflow,s->inflight.overflow);memset(&s->inflight,0,sizeof(s->inflight));
}
static void audit(StorageService*s,uint64_t now){
 if(!s->unsaved.count||s->audit_active||s->next_fault_id==UINT64_MAX||now>UINT64_MAX/1000)return;
 StorageServiceSlot*session=definition(s,RECORD_SESSION,0,0,0);if(!session)return;
 const char*device=NULL;unsigned count=atomic_load_explicit(&s->registry->published,memory_order_acquire);
 for(unsigned k=0;k<count;k++)if(s->registry->entries[k].value.identity.type==RECORD_SESSION){device=s->registry->entries[k].value.device_id;break;}
 StorageRequest r;if(!device||!storage_unsaved_record(device,s->queue->session_id,s->next_fault_id,now*1000,now*1000,&s->unsaved,&s->scratch,&r,s->payload,sizeof(s->payload))||!base(s,&r))return;
 StorageSubmit result=submit(s,&r,false,NULL,true);
 if(result.result==STORE_OK){s->inflight=s->unsaved;memset(&s->unsaved,0,sizeof(s->unsaved));s->audit_active=true;s->audit_ticket=result.ticket;s->next_fault_id++;}
}
void storage_service_step(StorageService*s,uint64_t now){
 if(!s||!s->queue)return;
 if(s->health.sealed)storage_writer_request_sync(s->queue->writer);
 storage_queue_step(s->queue,now);
 for(unsigned k=0;k<STORAGE_RECEIPTS;k++){
  StorageServiceSlot*x=&s->slots[k];if(!x->occupied)continue;StorageReceipt receipt;
  if(storage_queue_poll(s->queue,x->ticket,&receipt)!=STORE_OK)continue;
  x->state=receipt.state;if(x->state!=STORE_SYNCED&&x->state!=STORE_FAILED)continue;
  if(x->observer_failed)continue;
  if(!x->terminal_notified){
   x->terminal_notified=true;
   if(s->observer.terminal&&!s->observer.terminal(s->observer.context,x)){x->observer_failed=true;s->health.fatal=true;continue;}
  }
  if(!x->observed){
   x->observed=true;
   if(x->state==STORE_SYNCED)s->health.synced=satadd(s->health.synced,1);
   else{s->health.failed=satadd(s->health.failed,1);missing(s,&x->identity,x->identity.record_seq);}
   if(x->audit){if(x->state==STORE_FAILED)merge_back(s);else memset(&s->inflight,0,sizeof(s->inflight));s->audit_active=false;}
  }
  if(!x->definition&&storage_queue_ack(s->queue,x->ticket)==STORE_OK)x->occupied=false;
 }
 StorageQueueHealth h;if(storage_queue_health(s->queue,&h)!=STORE_OK)return;
 if(h.ready&&!h.live_jobs&&!s->health.fatal)audit(s,now);
 s->health.drained=h.ready&&s->health.sealed&&!s->health.fatal&&!s->health.pending&&!s->pcm_pending&&!h.live_jobs&&!s->unsaved.count&&!s->inflight.count&&!s->audit_active&&s->queue->writer->state==SW_READY;
}
StorageResult storage_service_release(StorageService*s,StorageTicket t){
 if(!s||!s->queue)return STORE_INVALID;
 for(unsigned k=0;k<STORAGE_RECEIPTS;k++){StorageServiceSlot*x=&s->slots[k];if(x->occupied&&x->definition&&same_ticket(t,x->ticket)){if(!x->observed)return STORE_BUSY;StorageResult r=storage_queue_ack(s->queue,t);if(r==STORE_OK)x->occupied=false;return r;}}
 return STORE_EXPIRED;
}
bool storage_service_recover(StorageService*s){return s&&s->queue&&!s->health.fatal&&storage_queue_recover(s->queue);}
bool storage_service_repair_tail(StorageService*s){return s&&s->queue&&!s->health.fatal&&storage_queue_repair_tail(s->queue);}

StorageSubmit storage_service_summary(StorageService *s,const MinuteSummary *m){
 if(!allowed(s))return reject(STORE_BUSY);
 StorageRequest r;if(!m||m->identity.session_id!=s->queue->session_id||!storage_summary_record(m,&r,s->payload,sizeof(s->payload)))return reject(STORE_INVALID);
 if(!base(s,&r))return reject(STORE_DEPENDENCY);
 return submit(s,&r,false,NULL,false);
}

bool storage_service_capture_ready(StorageService *s,uint64_t session_id,uint32_t config_id,uint64_t capture_id){
 if(!s||!session_id||!config_id||!capture_id||!s->registry||s->health.fatal||s->health.sealed)return false;
 bool session=false,config=false,capture=false;
 for(unsigned j=0;j<STORAGE_RECEIPTS;j++){StorageServiceSlot *x=&s->slots[j];if(!x->occupied||!x->definition)continue;if(!x->observed||x->state!=STORE_SYNCED)return false;
  if(x->identity.session_id!=session_id)continue;
  if(x->identity.type==RECORD_SESSION)session=true;
  if(x->identity.type==RECORD_CONFIG&&x->identity.config_id==config_id)config=true;
  if(x->identity.type==RECORD_CAPTURE&&x->identity.config_id==config_id&&x->identity.capture_id==capture_id)capture=true;
 }
 StoragePolicyCopy policy;return session&&config&&capture&&storage_registry_policy(s->registry,session_id,capture_id,config_id,0,&policy)==META_OK&&!policy.closed;
}

StorageSubmit storage_service_pcm_samples(StorageService*s,const StoragePcmSamples*pcm){
 if(!allowed(s))return reject(STORE_BUSY);
 if(!pcm)return reject(STORE_INVALID);
 StoragePolicyCopy p;if(pcm->session_id!=s->queue->session_id||storage_registry_policy(s->registry,pcm->session_id,pcm->capture_id,pcm->config_id,0,&p)!=META_OK)return reject(STORE_DEPENDENCY);
 StorageRequest r;if(!storage_pcm_samples_record(&p.policy,pcm,&r,s->payload,sizeof(s->payload)))return reject(STORE_INVALID);
 if(!data_dependencies(s,&r))return reject(STORE_DEPENDENCY);
 return submit(s,&r,false,NULL,false);
}
StorageSubmit storage_service_event_meta(StorageService*s,const StorageEventMeta*m){
 if(!allowed(s))return reject(STORE_BUSY);
 if(!m)return reject(STORE_INVALID);
 StoragePolicyCopy p;if(m->session_id!=s->queue->session_id||storage_registry_policy(s->registry,m->session_id,m->capture_id,m->config_id,0,&p)!=META_OK)return reject(STORE_DEPENDENCY);
 StorageRequest r;if(!storage_event_record(m,&r,s->payload,sizeof(s->payload)))return reject(STORE_INVALID);
 if(!data_dependencies(s,&r))return reject(STORE_DEPENDENCY);
 return submit(s,&r,false,NULL,false);
}
StorageSubmit storage_service_marker(StorageService*s,const LfnMarker*m){
 if(!allowed(s))return reject(STORE_BUSY);
 if(!m||m->start.session_id!=s->queue->session_id)return reject(STORE_INVALID);
 StoragePolicyCopy p;if(storage_registry_policy(s->registry,m->start.session_id,m->start.capture_id,m->start.config_id,0,&p)!=META_OK)return reject(STORE_DEPENDENCY);
 StorageRequest r;if(!storage_marker_record(p.device_id,m,&r,s->payload,sizeof(s->payload)))return reject(STORE_INVALID);
 if(!data_dependencies(s,&r))return reject(STORE_DEPENDENCY);
 return submit(s,&r,false,NULL,false);
}
