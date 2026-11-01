#include "event_runtime.h"
#include <string.h>
#define SECOND UINT64_C(48000)
#define SEGMENT (300*SECOND)
static StorageResult fail(EventRuntime*r,StorageResult e){if(!r->error)r->error=e;return r->error;}
static bool same(HistoryIdentity a,const LfnServiceIdentity*b){return a.session_id==b->session_id&&a.capture_id==b->capture_id&&a.config_id==b->config.config_id;}
static bool bit(const uint8_t*b,size_t n){return (b[n/8]&(1u<<(n%8)))!=0;}
static void setbit(uint8_t*b,size_t n){b[n/8]|=(uint8_t)(1u<<(n%8));}
static bool ticket_same(StorageTicket a,StorageTicket b){return a.session_id==b.session_id&&a.generation==b.generation&&a.slot==b.slot;}
static uint64_t ticks(EventRuntime*r){return r->now_us/125*6+(r->now_us%125)*6/125;}
static const char *reason(EventCloseReason why){static const char*names[]={"NORMAL","GAP","LOSS","SEQUENCE","IDENTITY","BAD_INPUT","STOP"};return (unsigned)why<7?names[why]:"BAD_INPUT";}
static void evidence(StorageEventMeta*m,const EventRecord*e){
 m->evidence_count=e->spectrum_count;
 if(e->spectrum_count){m->evidence_first_raw=e->onset_raw;m->evidence_end_raw=e->end_raw;strcpy(m->evidence_reason,"UNCLASSIFIED");}
 else{m->evidence_first_raw=m->evidence_end_raw=0;strcpy(m->evidence_reason,"INSUFFICIENT");}
}
static void details(EventRuntime*r){
 StorageEventMeta*m=&r->meta;m->trigger_count=0;m->trigger_total=r->record.trigger_count;
 for(unsigned k=0;k<r->record.trigger_stored;k++)if(r->record.triggers[k]<m->end_raw)m->triggers[m->trigger_count++]=r->record.triggers[k];
 m->triggers_truncated=m->trigger_total>m->trigger_count;evidence(m,&r->record);
}
static bool admission(void*ctx,const StorageServiceSlot*s,const StorageRequest*request){
 (void)request;EventRuntime*r=ctx;if(s->definition)return true;
 if(s->identity.type!=RECORD_PCM&&s->identity.type!=RECORD_EVENT_META)return true;
 if(!r->submitting||r->pending)return fail(r,STORE_INVALID)==STORE_OK;
 r->pending=true;r->pending_synced=false;r->ticket=s->ticket;r->receipt=s->identity;r->pending_file=r->pump->service->queue->file_id;
 r->pending_kind=s->identity.type;return true;
}
static bool terminal(void*ctx,const StorageServiceSlot*s){
 EventRuntime*r=ctx;if(s->definition|| (s->identity.type!=RECORD_PCM&&s->identity.type!=RECORD_EVENT_META))return true;
 const RecordIdentity*a=&r->receipt,*b=&s->identity;
 if(!r->pending||!ticket_same(r->ticket,s->ticket)||r->pending_file!=r->pump->service->queue->file_id||a->type!=b->type||a->record_seq!=b->record_seq||a->session_id!=b->session_id||a->capture_id!=b->capture_id||a->config_id!=b->config_id||a->first_raw!=b->first_raw||a->end_raw!=b->end_raw||s->state!=STORE_SYNCED)return fail(r,STORE_INVALID)==STORE_OK;
 r->pending_synced=true;return true;
}
bool event_runtime_bind_file(EventRuntime*r,StorageService*s,uint64_t id){
 if(!r||!s||r->pending||s->queue->session_id!=r->session||s->queue->file_id!=id)return false;
 s->observer=(StorageServiceObserver){r,admission,terminal};return true;
}
static void segment(EventRuntime*r,uint64_t first,uint32_t number){
 StorageEventMeta*m=&r->meta;memset(m,0,sizeof(*m));m->session_id=r->session;m->capture_id=r->record.identity.capture_id;m->config_id=r->record.identity.config.config_id;
 m->event_id=r->record.id;m->onset_raw=r->record.onset_raw;m->segment_no=number;m->actual_first=m->append_cursor=first;
 m->prehistory_requested_first=number?first:(r->record.onset_raw>=10*SECOND?r->record.onset_raw-10*SECOND:0);
 m->prehistory_short=first>m->prehistory_requested_first;m->persist_revision=1;m->state=EVENT_META_OPEN;
 strcpy(m->close_reason,"NONE");strcpy(m->missing_ref_reason,"NONE");strcpy(m->evidence_reason,"INSUFFICIENT");
 StorageMetadataRegistry*registry=r->pump->service->registry;unsigned n=atomic_load_explicit(&registry->published,memory_order_acquire);
 for(unsigned k=0;k<n;k++)if(registry->entries[k].value.identity.type==RECORD_SESSION){memcpy(m->device_id,registry->entries[k].value.device_id,64);break;}
 r->segment_limit=first>UINT64_MAX-SEGMENT?UINT64_MAX:first+SEGMENT;r->meta_open=true;r->missing=false;
 m->end_raw=r->confirmed_end<r->segment_limit?r->confirmed_end:r->segment_limit;if(m->end_raw<first)m->end_raw=first;details(r);
}
static bool open_event(EventRuntime*r,const EventRecord*e){
 if(r->active)return fail(r,STORE_EXHAUSTED)==STORE_OK;
 r->record=*e;r->confirmed_end=e->end_raw;r->active=true;r->closing=false;r->close_since=0;
 uint64_t first=e->onset_raw>=10*SECOND?e->onset_raw-10*SECOND:0;
 HistoryWatermark w={0};bool have=history_watermark(r->history,&w)&&w.active&&w.have_samples&&same(w.identity,&e->identity);
 if(have){if(first<w.base_raw)first=w.base_raw;/* Preserve the grid even near raw zero. */
  uint64_t rem=(first-w.base_raw)%48;if(rem)first+=48-rem;
  r->frozen=w;r->prefetch_first=first;r->prefetch_end=w.end_raw>first?w.end_raw:first;r->prefetch_cursor=first;
  if(r->prefetch_end-first>HISTORY_SAMPLES*UINT64_C(48)){r->prefetch_first=r->prefetch_cursor=r->prefetch_end-HISTORY_SAMPLES*UINT64_C(48);}
 }else{first=e->onset_raw;r->prefetch_first=r->prefetch_end=r->prefetch_cursor=first;}
 memset(r->prefetch_valid,0,sizeof(r->prefetch_valid));segment(r,first,0);return true;
}
static bool changes(EventRuntime*r){
 EventChange c;for(unsigned n=0;n<EVENT_FEED_SLOTS;n++){
  if(r->feed){
   if(event_feed_error(r->feed)!=EVENT_OK)return fail(r,STORE_EXHAUSTED)==STORE_OK;
   if(!event_feed_peek(r->feed,&c))break;
   if(c.kind==EVENT_OPEN&&(r->active||!storage_service_capture_ready(r->pump->service,r->session,c.event.identity.config.config_id,c.event.identity.capture_id)))break;
   (void)event_feed_take(r->feed,&c);
  }else if(!event_engine_take(&r->engine,&c))break;
  if(c.kind==EVENT_OPEN){if(!open_event(r,&c.event))return false;}
  else if(r->active&&r->record.id==c.event.id){r->record=c.event;if(c.kind==EVENT_CLOSE){r->closing=true;r->confirmed_end=c.event.end_raw;r->close_since=r->now_us;}}
  else if(c.kind==EVENT_EVIDENCE){for(unsigned k=0;k<EVENT_PENDING_SLOTS;k++)if(r->late[k].used&&r->late[k].meta.event_id==c.event.id){evidence(&r->late[k].meta,&c.event);r->late[k].dirty=true;break;}}
 }
 if(r->active&&!r->closing){
  LfnServiceIdentity id;uint64_t end;bool have=false;
  if(r->feed){EventFeedProgress p;if(event_feed_progress(r->feed,&p)&&p.event_id==r->record.id){id=p.identity;end=p.end_raw;have=true;}}
  else have=event_engine_confirmed_end(&r->engine,&id,&end);
  if(have&&same((HistoryIdentity){r->session,r->record.identity.capture_id,r->record.identity.config.config_id},&id))r->confirmed_end=end;
 }
 return true;
}
static StorageResult preflight(void*ctx,const StorageMultiSource*s){
 EventRuntime*r=ctx;if(r->error)return r->error;
 /* A logical close must be durable before a later ENERGY can open the next
  * event. PCM copying remains capture-owned and keeps running while draining. */
 if(!r->feed&&r->active&&r->closing&&s->kind==PUMP_ENERGY)return STORE_BUSY;
 return r->delegate.preflight?r->delegate.preflight(r->delegate.context,s):STORE_OK;
}
static void cancel(void*ctx){EventRuntime*r=ctx;if(r->delegate.cancel)r->delegate.cancel(r->delegate.context);}
static bool commit(void*ctx,const StorageMultiSource*s,StoragePump*p,StoragePumpTicket t){
 EventRuntime*r=ctx;if(r->delegate.commit&&!r->delegate.commit(r->delegate.context,s,p,t))return false;
 if(r->feed)return true; /* SD delivery loss is distinct from capture loss. */
 EventResult result=EVENT_OK;
 if(s->kind==PUMP_ENERGY){const LfnServiceMessage*m=s->value.energy;
  if(m->kind==SERVICE_DELIVERY_LOSS||!m->identity.config.mode)result=event_on_energy(&r->engine,m,ticks(r));
 }else if(s->kind==PUMP_PSD){const LfnSpectrum*psd=s->value.psd;if(psd->valid&&psd->flags==LFN_VALID)result=event_on_spectrum(&r->engine,psd,ticks(r));}
 else if(s->kind==PUMP_INGRESS_LOSS)result=event_engine_finish(&r->engine,EVENT_LOSS,ticks(r));
 if(result!=EVENT_OK&&result!=EVENT_NO_MATCH)return fail(r,STORE_INVALID)==STORE_OK;
 return changes(r);
}
static StorageResult before_close(void*ctx){
 EventRuntime*r=ctx;if(r->error)return r->error;
 if(!r->stopping){
  if(r->feed){if(!event_feed_closed(r->feed))return STORE_BUSY;}
  else if(event_engine_finish(&r->engine,EVENT_STOP,ticks(r))!=EVENT_OK||!changes(r))return fail(r,STORE_INVALID);
  r->stopping=true;
 }
 StorageResult sr=r->delegate.before_close?r->delegate.before_close(r->delegate.context):STORE_OK;
 if(sr!=STORE_OK&&sr!=STORE_BUSY)return sr;
 event_runtime_step(r,r->now_us);
 if(r->error)return r->error;
 bool dirty=false;for(unsigned k=0;k<EVENT_PENDING_SLOTS;k++)dirty|=r->late[k].dirty;
 return r->active||r->pending||r->roll_pending||dirty||(r->feed&&event_feed_pending(r->feed))?STORE_BUSY:sr;
}
bool event_runtime_init(EventRuntime*r,uint64_t session,PcmHistory*h,SummaryRuntime*s,const EventRuntimeRollOps*roll){
 if(!r||!session||!h)return false;
 memset(r,0,sizeof(*r));r->session=session;r->history=h;r->summary=s;r->next_ordinal=1;r->submitted_late=-1;if(roll)r->roll=*roll;return event_engine_init(&r->engine,5e-10);
}
bool event_runtime_use_feed(EventRuntime*r,EventFeed*f){
 if(!r||!f||r->generation||r->active||f->session!=r->session||event_feed_error(f)!=EVENT_OK)return false;
 r->feed=f;return true;
}
bool event_runtime_attach(void*ctx,StorageMultiSink*s,CaptureStorageRoute*route,StoragePump*const*p,unsigned count,uint64_t generation){
 EventRuntime*r=ctx;if(!r||!s||!route||count!=4||r->active||r->pending||r->error)return false;
 if(r->generation){
  if(r->feed&&(!event_feed_closed(r->feed)||event_feed_pending(r->feed)))return false;
  for(unsigned k=0;k<EVENT_PENDING_SLOTS;k++)if(r->late[k].dirty)return false;
  if(!r->feed)event_engine_expire_pending(&r->engine);
  memset(r->late,0,sizeof(r->late));
 }
 if(r->summary&&!summary_runtime_attach(r->summary,s,route,p,count,generation))return false;
 r->delegate=s->observer;r->route=route;r->pump=p[3];r->generation=generation;r->stopping=false;
 if(!event_runtime_bind_file(r,r->pump->service,r->pump->service->queue->file_id))return false;
 s->observer=(StorageMultiObserver){r,preflight,commit,cancel,before_close};return true;
}
static bool submit_meta(EventRuntime*r,StorageEventMeta*m,int late){
 r->submitted_meta=*m;r->submitted_late=late;r->submitting=true;StorageSubmit s=storage_service_event_meta(r->pump->service,&r->submitted_meta);r->submitting=false;
 if(s.result==STORE_OK){if(!r->pending||!ticket_same(s.ticket,r->ticket))fail(r,STORE_INVALID);storage_writer_request_sync(r->pump->service->queue->writer);return true;}
 if(s.result!=STORE_BUSY&&s.result!=STORE_CAPACITY&&s.result!=STORE_DEPENDENCY)fail(r,s.result);
 return false;
}
static void completed(EventRuntime*r){
 if(!r->pending||!r->pending_synced)return;
 r->pending=false;r->pending_synced=false;
 if(r->pending_kind==RECORD_PCM){StorageEventMeta*m=&r->meta;RecordIdentity*i=&r->receipt;
  if(m->pcm_count==32){fail(r,STORE_EXHAUSTED);return;}
  m->pcm[m->pcm_count++]=(StorageEventPcmRef){r->pending_file,i->record_seq,i->first_raw,i->end_raw,48,(uint32_t)((i->end_raw-i->first_raw)/48)};m->valid_sample_count+=(i->end_raw-i->first_raw)/48;return;
 }
 StorageEventLink link={r->pending_file,r->receipt.record_seq};StorageEventMeta*sent=&r->submitted_meta;
 if(r->submitted_late>=0){EventLateRecord*l=&r->late[r->submitted_late];l->meta.prev_meta=link;l->meta.persist_revision=sent->persist_revision+1;l->dirty=l->meta.evidence_count>sent->evidence_count;return;}
 r->meta.prev_meta=link;r->meta.persist_revision=sent->persist_revision+1;r->meta.pcm_count=r->meta.gap_count=0;r->meta_open=false;
 if(sent->state==EVENT_META_CLOSED||sent->state==EVENT_META_INCOMPLETE){
  r->segments_closed++;
  if(!sent->event_closed){r->roll_pending=true;r->roll_started=false;return;}
  unsigned at=EVENT_PENDING_SLOTS;for(unsigned k=0;k<EVENT_PENDING_SLOTS;k++)if(!r->late[k].used||r->now_us-r->late[k].closed_us>UINT64_C(12288000)){at=k;break;}
  if(at<EVENT_PENDING_SLOTS){r->late[at]=(EventLateRecord){*sent,true,false,r->now_us};r->late[at].meta.prev_meta=link;r->late[at].meta.persist_revision++;r->late[at].meta.pcm_count=r->late[at].meta.gap_count=0;
   if(r->record.spectrum_count>sent->evidence_count){evidence(&r->late[at].meta,&r->record);r->late[at].dirty=true;}}
  r->events_closed++;r->active=r->closing=false;
 }
}
static void add_gap(EventRuntime*r,uint64_t first,uint64_t end){
 StorageEventMeta*m=&r->meta;r->missing=true;
 if(m->gap_count&&m->gaps[m->gap_count-1].end_raw==first)m->gaps[m->gap_count-1].end_raw=end;
 else if(m->gap_count<32)m->gaps[m->gap_count++]=(StorageEventGap){first,end};else m->gaps_truncated=true;
 m->append_cursor=end;
}
static bool prefetch(EventRuntime*r){
 if(r->prefetch_cursor>=r->prefetch_end)return false;
 size_t n=(size_t)((r->prefetch_end-r->prefetch_cursor)/48);if(n>4096)n=4096;HistoryReadResult result;
 size_t at=(size_t)((r->prefetch_cursor-r->prefetch_first)/48);
 bool ok=history_copy(r->history,&r->frozen,r->prefetch_cursor,n,r->samples,r->valid,&result);
 for(size_t k=0;k<n;k++)if(ok&&bit(r->valid,k)){r->prefetch[at+k]=r->samples[k];setbit(r->prefetch_valid,at+k);}
 r->prefetch_cursor+=n*48;return true;
}
void event_runtime_step(EventRuntime*r,uint64_t now){
 if(!r||r->error)return;
 if(now<r->now_us){fail(r,STORE_INVALID);return;}r->now_us=now;
 completed(r);if(r->error)return;
 if(r->feed&&(!r->pump||!changes(r)))return;
 if(r->pending){storage_writer_request_sync(r->pump->service->queue->writer);return;}
 if(r->roll_pending){
  if(!r->roll.begin||!r->roll.status){fail(r,STORE_CAPACITY);return;}
  if(!r->roll_started){if(r->next_ordinal==UINT64_C(1)<<63){fail(r,STORE_EXHAUSTED);return;}StorageResult e=r->roll.begin(r->roll.context,r->next_ordinal,now);if(e==STORE_BUSY)return;r->next_ordinal++;if(e!=STORE_OK){fail(r,e);return;}r->roll_started=true;return;}
  uint64_t id;StorageResult e=r->roll.status(r->roll.context,&id);if(e==STORE_BUSY)return;if(e!=STORE_OK||id!=r->pump->service->queue->file_id){fail(r,e==STORE_OK?STORE_INVALID:e);return;}
  uint64_t first=r->meta.append_cursor;uint32_t segment_no=r->meta.segment_no+1;r->roll_pending=false;segment(r,first,segment_no);
 }
 if(!r->active){for(unsigned k=0;k<EVENT_PENDING_SLOTS;k++)if(r->late[k].dirty){submit_meta(r,&r->late[k].meta,(int)k);return;}return;}
 StorageEventMeta*m=&r->meta;m->end_raw=r->confirmed_end<r->segment_limit?r->confirmed_end:r->segment_limit;details(r);
 if(r->meta_open){submit_meta(r,m,-1);return;}
 if(prefetch(r))return;
 bool end=r->closing&&m->end_raw==r->confirmed_end,segment_end=m->end_raw==r->segment_limit;
 if(m->pcm_count==32||m->gap_count==32){m->state=EVENT_META_APPEND;strcpy(m->close_reason,"NONE");submit_meta(r,m,-1);return;}
 uint64_t safe=m->end_raw;HistoryWatermark w;bool have=history_watermark(r->history,&w)&&w.active&&w.have_samples&&same(w.identity,&r->record.identity);
 if(have&&safe>w.end_raw)safe=w.end_raw;
 if(!have)safe=m->append_cursor;
 if(m->append_cursor<safe){
  size_t count=(size_t)((safe-m->append_cursor)/48);if(count>4096)count=4096;
  if(count<4096&&!end&&!segment_end)return;
  uint64_t first=m->append_cursor;size_t n=count;
  memset(r->valid,0,sizeof(r->valid));
  if(first>=r->prefetch_first&&first<r->prefetch_end){if(n>(r->prefetch_end-first)/48)n=(size_t)((r->prefetch_end-first)/48);size_t at=(size_t)((first-r->prefetch_first)/48);for(size_t k=0;k<n;k++)if(bit(r->prefetch_valid,at+k)){r->samples[k]=r->prefetch[at+k];setbit(r->valid,k);}}
  else {HistoryReadResult result;if(!history_copy(r->history,&w,first,n,r->samples,r->valid,&result))memset(r->valid,0,sizeof(r->valid));}
  bool good=bit(r->valid,0);size_t run=1;while(run<n&&bit(r->valid,run)==good)run++;
  if(!good){add_gap(r,first,first+run*48);return;}
  StoragePcmSamples pcm={r->session,r->record.identity.capture_id,first,r->record.identity.config.config_id,LFN_VALID,run,r->samples};
  r->submitting=true;StorageSubmit s=storage_service_pcm_samples(r->pump->service,&pcm);r->submitting=false;
  if(s.result==STORE_OK){m->append_cursor+=run*48;storage_writer_request_sync(r->pump->service->queue->writer);}
  else if(s.result!=STORE_BUSY&&s.result!=STORE_CAPACITY&&s.result!=STORE_DEPENDENCY)fail(r,s.result);
  return;
 }
 if(m->append_cursor<m->end_raw){if((end||segment_end)&&r->closing&&now-r->close_since>=UINT64_C(1000000)){add_gap(r,m->append_cursor,m->end_raw);m->tail_missing=true;}return;}
 if(end||segment_end){m->state=r->missing||m->tail_missing?EVENT_META_INCOMPLETE:EVENT_META_CLOSED;m->event_closed=end;strcpy(m->close_reason,end?reason(r->record.reason):"SEGMENT_LIMIT");strcpy(m->missing_ref_reason,r->missing?"HISTORY_GAP":"NONE");submit_meta(r,m,-1);}
}
