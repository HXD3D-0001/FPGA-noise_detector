#include "storage_segment.h"
#include <string.h>
uint32_t storage_segment_trace;
static bool owned(StorageSegment *s){return s&&s->is_owner&&s->is_owner(s->owner_context);}
static bool config_equal(const PlCaptureConfig *a,const PlCaptureConfig *b){return a->config_id==b->config_id&&a->mode==b->mode&&a->source==b->source&&a->channel==b->channel&&a->pattern==b->pattern;}
static bool ticket_equal(StorageTicket a,StorageTicket b){return a.session_id==b.session_id&&a.generation==b.generation&&a.slot==b.slot;}
static bool observed(StorageService *s,StorageTicket t){for(unsigned k=0;k<STORAGE_RECEIPTS;k++){StorageServiceSlot *x=&s->slots[k];if(x->occupied&&x->definition&&ticket_equal(x->ticket,t))return x->observed&&x->state==STORE_SYNCED;}return false;}
/* 0 absent, 1 exact retained definition, -1 conflicting/non-retained identity. */
static int existing(StorageService *s,const StorageDefinitionCopy *d,StorageTicket *t){
 unsigned n=atomic_load(&s->registry->published);
 for(unsigned k=0;k<n;k++){StorageMetadataEntry *e=&s->registry->entries[k];RecordIdentity *a=&e->value.identity;
  if(a->type!=d->identity.type||a->session_id!=d->identity.session_id||a->config_id!=d->identity.config_id||a->capture_id!=d->identity.capture_id)continue;
  if(e->bytes!=d->bytes||memcmp(e->payload,d->payload,d->bytes))return -1;
  for(unsigned j=0;j<STORAGE_RECEIPTS;j++){StorageServiceSlot *x=&s->slots[j];if(x->occupied&&x->definition&&x->identity.record_seq==a->record_seq){*t=x->ticket;return 1;}}return -1;
 }
 return 0;
}
bool storage_segment_init(StorageSegment *s,StorageService *const *services,unsigned n,uint64_t session,uint64_t seed,bool (*owner)(void *),void *ctx){
 if(!s||!services||!n||n>STORAGE_FAMILY_FILES||!session||!seed||seed==UINT64_MAX||!owner||!owner(ctx))return false;
 StorageService *copy[STORAGE_FAMILY_FILES]={0};for(unsigned k=0;k<n;k++){if(!services[k]||!services[k]->queue||!services[k]->registry||services[k]->queue->session_id!=session)return false;for(unsigned j=0;j<k;j++)if(services[j]==services[k])return false;copy[k]=services[k];}
 memset(s,0,sizeof(*s));memcpy(s->services,copy,sizeof(copy));s->count=n;s->session_id=session;s->next_request=seed;s->is_owner=owner;s->owner_context=ctx;return true;
}
StorageResult storage_segment_begin(StorageSegment *s,const CaptureRouteFacts *f,const StorageConfigFacts *cfg,const StorageCaptureFacts *clock,uint64_t *token){
 if(!owned(s)||!f||!cfg||!clock||!token)return STORE_INVALID;
 if(s->state!=SEGMENT_IDLE)return STORE_BUSY;
 if(s->next_request==UINT64_MAX)return STORE_EXHAUSTED;
 storage_segment_trace=0;
 if(f->version<s->facts_version||(s->facts_version&&f->version==s->facts_version&&(f->identity.capture_id!=s->definitions[1].identity.capture_id||f->identity.config.config_id!=s->definitions[1].identity.config_id))){storage_segment_trace=1;return STORE_INVALID;}
 if(!f->version){storage_segment_trace=2;return STORE_INVALID;}
 if(!f->started_valid){storage_segment_trace=3;return STORE_INVALID;}
 if(!f->active_valid){storage_segment_trace=4;return STORE_INVALID;}
 if(!f->capture_valid){storage_segment_trace=5;return STORE_INVALID;}
 if(f->software_drained){storage_segment_trace=6;return STORE_INVALID;}
 if(f->identity.session_id!=s->session_id){storage_segment_trace=7;return STORE_INVALID;}
 if(cfg->session_id!=s->session_id){storage_segment_trace=8;return STORE_INVALID;}
 if(!config_equal(&cfg->config,&f->identity.config)){storage_segment_trace=9;return STORE_INVALID;}
 if(f->started.capture_id!=f->identity.capture_id){storage_segment_trace=10;return STORE_INVALID;}
 if(f->active.plan.capture_id!=f->identity.capture_id){storage_segment_trace=11;return STORE_INVALID;}
 if(f->active.plan.session_id!=s->session_id){storage_segment_trace=12;return STORE_INVALID;}
 if(clock->closed){storage_segment_trace=13;return STORE_INVALID;}
 if(clock->revision!=1){storage_segment_trace=14;return STORE_INVALID;}
 if(!clock->device_id||!cfg->device_id){storage_segment_trace=15;return STORE_INVALID;}
 if(strcmp(clock->device_id,cfg->device_id)){storage_segment_trace=16;return STORE_INVALID;}
 StorageCaptureFacts cap=*clock;cap.config=cfg->config;cap.started=f->started;cap.active=f->active;
 if(cap.schema_version==2)cap.time_pair=f->time_pair;
 StorageRequest a,b;
 if(!storage_build_config(&s->builder,cfg,&a,s->definitions[0].payload,STORAGE_METADATA_BYTES)){storage_segment_trace=20;return STORE_INVALID;}
 if(!storage_build_capture(&s->builder,&cap,&b,s->definitions[1].payload,STORAGE_METADATA_BYTES)){storage_segment_trace=21;return STORE_INVALID;}
 s->definitions[0].identity=a.identity;s->definitions[0].bytes=a.payload_bytes;s->definitions[1].identity=b.identity;s->definitions[1].bytes=b.payload_bytes;
 for(unsigned k=0;k<s->count;k++){
  StorageService *v=s->services[k];
  if(v->health.fatal){storage_segment_trace=30;return STORE_INVALID;}
  if(v->health.sealed){storage_segment_trace=31;return STORE_INVALID;}
  bool session=false;
  for(unsigned j=0;j<STORAGE_RECEIPTS;j++){StorageServiceSlot *x=&v->slots[j];if(x->occupied&&x->definition&&x->identity.type==RECORD_SESSION&&x->identity.session_id==s->session_id&&x->observed&&x->state==STORE_SYNCED)session=true;}
  if(!session){storage_segment_trace=32;return STORE_DEPENDENCY;}
  unsigned added=0;for(unsigned j=0;j<2;j++){StorageTicket t;int match=existing(v,&s->definitions[j],&t);if(match<0){storage_segment_trace=33;return STORE_INVALID;}if(!match)added++;}
  if(atomic_load(&v->registry->published)+added>STORAGE_METADATA_ENTRIES){storage_segment_trace=34;return STORE_CAPACITY;}
 }
 s->file=s->record=0;s->facts_version=f->version;s->request_id=s->next_request++;s->error=STORE_OK;s->state=SEGMENT_SUBMIT;*token=s->request_id;return STORE_OK;
}
static void fail(StorageSegment *s,StorageResult error){s->state=SEGMENT_FAILED;s->error=error;}
void storage_segment_step(StorageSegment *s){
 if(!owned(s)||(s->state!=SEGMENT_SUBMIT&&s->state!=SEGMENT_WAIT))return;
 StorageService *v=s->services[s->file];if(v->health.fatal||v->health.sealed){fail(s,STORE_INVALID);return;}
 if(s->state==SEGMENT_SUBMIT){StorageDefinitionCopy *d=&s->definitions[s->record];int match=existing(v,d,&s->ticket);if(match<0){fail(s,STORE_INVALID);return;}if(!match){StorageSubmit r=storage_service_metadata(v,&d->identity,d->payload,d->bytes);if(r.result==STORE_BUSY||r.result==STORE_CAPACITY)return;if(r.result!=STORE_OK){fail(s,r.result);return;}s->ticket=r.ticket;}s->state=SEGMENT_WAIT;return;}
 StorageReceipt r;StorageResult result=storage_queue_poll(v->queue,s->ticket,&r);if(result==STORE_BUSY)return;if(result!=STORE_OK||r.state==STORE_FAILED){fail(s,result==STORE_OK?STORE_INVALID:result);return;}if(r.state!=STORE_SYNCED||!observed(v,s->ticket))return;
 if(++s->record==2){s->record=0;if(++s->file==s->count){s->state=SEGMENT_SYNCED;return;}}s->state=SEGMENT_SUBMIT;
}
StorageResult storage_segment_ack(StorageSegment *s,uint64_t token){if(!owned(s))return STORE_INVALID;if(!token||token!=s->request_id)return STORE_EXPIRED;if(s->state!=SEGMENT_SYNCED)return STORE_BUSY;s->state=SEGMENT_IDLE;s->request_id=0;return STORE_OK;}
