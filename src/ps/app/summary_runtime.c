#include "summary_runtime.h"
#include "capture_time_contract.h"
#include <string.h>
#include <math.h>
static StorageResult fail(SummaryRuntime*r,StorageResult e){if(r->error==STORE_OK)r->error=e;return r->error;}
static bool ticket_eq(StorageTicket a,StorageTicket b){return a.session_id==b.session_id&&a.generation==b.generation&&a.slot==b.slot;}
static bool identity_eq(const RecordIdentity*a,const RecordIdentity*b){return a->type==b->type&&a->flags==b->flags&&a->config_id==b->config_id&&a->session_id==b->session_id&&a->capture_id==b->capture_id&&a->first_raw==b->first_raw&&a->end_raw==b->end_raw&&a->record_seq==b->record_seq;}
static bool current(SummaryRuntime*r,const RecordIdentity*i){return r->begun&&i->session_id==r->session&&i->capture_id==r->identity.capture_id&&i->config_id==r->identity.config_id;}
/* Continuous MON PCM, PSD, and SUMMARY only. EVENT history may be written
 * long after the summary cursor; it never revises continuous coverage. */
static bool source_role(unsigned file,RecordType type){return (file==0&&type==RECORD_PCM)||(file==1&&type==RECORD_PSD)||(file==2&&type==RECORD_MINUTE_SUMMARY);}
/* All source receipts fence EOF, including RAW and identity-free notices.
 * Statistics remain restricted to source_role; EVENT owns its own lifecycle. */
static bool receipt_role(unsigned file,RecordType type){return (file<2&&(type==RECORD_ENERGY||type==RECORD_PCM||type==RECORD_PSD||type==RECORD_FAULT))||(file==2&&type==RECORD_MINUTE_SUMMARY);}

static void reclaim(SummaryRanges *u,uint64_t cursor){unsigned n=0;for(unsigned k=0;k<u->count;k++)if(u->ranges[k].end>cursor){u->ranges[n]=u->ranges[k];if(u->ranges[n].first<cursor)u->ranges[n].first=cursor;n++;}u->count=n;}
static bool range_add(SummaryRanges*u,uint64_t first,uint64_t end){
 if(first>=end)return true;
 unsigned a=0;while(a<u->count&&u->ranges[a].end<first)a++;unsigned b=a;
 while(b<u->count&&u->ranges[b].first<=end){if(u->ranges[b].first<first)first=u->ranges[b].first;if(u->ranges[b].end>end)end=u->ranges[b].end;b++;}
 if(a==b&&u->count==SUMMARY_RUNTIME_RANGES)return false;
 memmove(&u->ranges[a+1],&u->ranges[b],(u->count-b)*sizeof(u->ranges[0]));u->count=u->count-(b-a)+1;u->ranges[a]=(SummaryRange){first,end};return true;
}
static bool contains(const SummaryRanges*u,uint64_t first,uint64_t end){for(unsigned k=0;k<u->count;k++)if(u->ranges[k].first<=first&&u->ranges[k].end>=end)return true;return false;}
static bool room(const SummaryRanges*u,uint64_t first,uint64_t end){if(u->count<SUMMARY_RUNTIME_RANGES)return true;for(unsigned k=0;k<u->count;k++)if(u->ranges[k].end>=first&&u->ranges[k].first<=end)return true;return false;}
static uint64_t boundary(const SummaryRanges*u,uint64_t cursor,uint64_t end){for(unsigned k=0;k<u->count;k++){if(u->ranges[k].first>cursor&&u->ranges[k].first<end)end=u->ranges[k].first;if(u->ranges[k].end>cursor&&u->ranges[k].end<end)end=u->ranges[k].end;}return end;}
static void prune(SummaryRuntime*r){reclaim(&r->pcm_valid,r->cursor);reclaim(&r->pcm_saved,r->cursor);reclaim(&r->available,r->cursor);for(unsigned k=0;k<SUMMARY_RUNTIME_PSD;k++)if(r->psd[k].used&&r->psd[k].end<=r->cursor)r->psd[k].used=false;}
static const StorageServiceSlot *latest(StorageService*s,RecordType type,uint64_t cap,uint32_t cfg){const StorageServiceSlot*best=NULL;for(unsigned k=0;k<STORAGE_RECEIPTS;k++){const StorageServiceSlot*x=&s->slots[k];if(!x->occupied||!x->definition||x->identity.type!=type||(cap&&x->identity.capture_id!=cap)||(cfg&&x->identity.config_id!=cfg))continue;if(!best||x->identity.record_seq>best->identity.record_seq)best=x;}return best;}
static const StorageMetadataEntry *entry(StorageService*s,uint64_t seq){unsigned n=atomic_load_explicit(&s->registry->published,memory_order_acquire);for(unsigned k=0;k<n;k++)if(s->registry->entries[k].value.identity.record_seq==seq)return &s->registry->entries[k];return NULL;}
static StorageResult ensure(SummaryRuntime*r,uint64_t capture,uint32_t cfg){
 if(r->error)return r->error;
 bool same=r->begun&&!r->closed&&r->identity.capture_id==capture&&r->identity.config_id==cfg;
 if(r->begun&&!r->closed&&!same)return fail(r,STORE_INVALID);
 if(r->closed_capture==capture)return fail(r,STORE_EXPIRED);
 StorageService*s=r->pumps[2]->service;const StorageServiceSlot*d[3]={latest(s,RECORD_SESSION,0,0),latest(s,RECORD_CONFIG,0,cfg),latest(s,RECORD_CAPTURE,capture,cfg)};
 const StorageMetadataEntry*e[3];
 for(unsigned k=0;k<3;k++){if(!d[k])return STORE_DEPENDENCY;if(d[k]->state==STORE_FAILED)return fail(r,STORE_INVALID);if(!d[k]->observed||d[k]->state!=STORE_SYNCED)return STORE_DEPENDENCY;e[k]=entry(s,d[k]->identity.record_seq);if(!e[k]||!identity_eq(&e[k]->value.identity,&d[k]->identity))return fail(r,STORE_INVALID);}
 if(same){for(unsigned k=0;k<3;k++)if(d[k]->identity.record_seq!=r->definition_sequence[k]||!ticket_eq(d[k]->ticket,r->definitions[k]))return fail(r,STORE_INVALID);return STORE_OK;}
 if(e[1]->value.config.mode)return STORE_OK;
 if(!e[1]->value.summary_histogram)return fail(r,STORE_INVALID);
 StorageMetadata *cap=&r->capture_definition;
 if(storage_registry_capture(s->registry,r->session,capture,cfg,e[2]->value.revision,0,cap)!=META_OK||cap->identity.record_seq!=d[2]->identity.record_seq||cap->closed||cap->time_quality!=STORAGE_TIME_NOMINAL_PAIRED||!capture_time_nominal_valid(&cap->time_pair))return fail(r,STORE_INVALID);
 SummaryIdentity id={0};id.session_id=r->session;id.capture_id=capture;id.config_id=cfg;id.test_source=e[1]->value.config.source==PL_CAPTURE_TEST;memcpy(id.device_id,e[0]->value.device_id,64);
 if(!metadata_json_parse(&r->scratch,e[1]->payload,e[1]->bytes)||!metadata_json_string(&r->scratch,metadata_json_field(&r->scratch,0,"band_definition_id"),id.band_definition_id,64))return fail(r,STORE_INVALID);
 if(!minute_summary_init_v2(&r->core,&id,&cap->time_pair,e[0]->value.start_ns,cap->first_valid_raw,cap->revision,UINT64_C(60000000000),r->next_id))return fail(r,STORE_INVALID);
 r->identity=id;r->first_raw=r->cursor=cap->first_valid_raw;r->fft_first=cap->first_raw[0];r->fft_seq=cap->first_seq[0];
 for(unsigned k=0;k<3;k++){r->definitions[k]=d[k]->ticket;r->definition_sequence[k]=d[k]->identity.record_seq;r->frontier[k]=r->first_raw;r->last_first[k]=r->first_raw;r->seen[k]=false;}
 memset(r->psd,0,sizeof(r->psd));memset(&r->pcm_valid,0,sizeof(r->pcm_valid));memset(&r->pcm_saved,0,sizeof(r->pcm_saved));memset(&r->available,0,sizeof(r->available));
 r->begun=true;r->closed=false;r->eof=false;r->source_hold=false;return STORE_OK;
}
static StorageResult preflight(void*ctx,const StorageMultiSource*src){
 SummaryRuntime*r=ctx;if(r->error)return r->error;r->staged=false;uint64_t cap=0,first=0,end=0,seq=0;uint32_t cfg=0;unsigned stream=0;PacketView v;
 if(src->kind==PUMP_ENERGY){const LfnServiceMessage*m=src->value.energy;if(m->kind!=SERVICE_ENERGY||m->energy.gap)return STORE_OK;cap=m->identity.capture_id;cfg=m->identity.config.config_id;first=m->energy.record.first_raw;if(first>UINT64_MAX-12288)return fail(r,STORE_EXHAUSTED);end=first+12288;}
 else if(src->kind==PUMP_PSD){const LfnSpectrum*s=src->value.psd;cap=s->capture_id;cfg=s->config_id;first=s->first_raw;end=s->end_raw;seq=s->stream_seq;stream=1;}
 else if(src->kind==PUMP_PCM){if(!packet_decode(src->value.pcm,src->bytes,&v))return fail(r,STORE_INVALID);if(v.header.type==LFN_RAW)return STORE_OK;cap=v.header.capture_id;cfg=v.header.config_id;first=v.header.first_raw_sample_index;end=v.end_raw;stream=2;}
 else return STORE_OK;
 StorageResult result=ensure(r,cap,cfg);if(result!=STORE_OK)return result;
 if(!r->begun||r->identity.capture_id!=cap||r->identity.config_id!=cfg)return STORE_OK;
 if(r->closed||r->eof||first<r->first_raw||end<=first||(first-r->first_raw)%48||(end-first)%48||(r->seen[stream]&&(first<r->last_first[stream]||(stream==1&&first==r->last_first[stream]))))return fail(r,STORE_INVALID);
 prune(r);
 if(!room(&r->available,first,end)||(stream==2&&v.header.flags==LFN_VALID&&!room(&r->pcm_valid,first,end)))return fail(r,STORE_EXHAUSTED);
 if(stream==1){
  unsigned at=0;while(at<SUMMARY_RUNTIME_PSD&&r->psd[at].used)at++;if(at==SUMMARY_RUNTIME_PSD)return fail(r,STORE_EXHAUSTED);
  if(end-first!=196608||first<r->fft_first||(first-r->fft_first)%98304||seq<r->fft_seq||seq-r->fft_seq!=(first-r->fft_first)/98304)return fail(r,STORE_INVALID);
  SummaryPsdFact*p=&r->staged_psd;memset(p,0,sizeof(*p));p->first=first+98304;p->end=end;p->seq=seq;p->flags=src->value.psd->flags;p->used=true;
  for(unsigned k=0;k<10;k++){double center=1000*pow(10.0,((double)k-15)/10),ratio=pow(10.0,0.05);if(!lfn_spectrum_band_power(src->value.psd->psd,LFN_SPECTRUM_BINS,1000,center/ratio,center*ratio,&p->power[k]))return fail(r,STORE_INVALID);}
 }
 r->staged=true;return STORE_OK;
}
static void cancel(void*ctx){((SummaryRuntime*)ctx)->staged=false;}
static bool commit(void*ctx,const StorageMultiSource*src,StoragePump*p,StoragePumpTicket ticket){
 (void)p;(void)ticket;SummaryRuntime*r=ctx;if(!r->staged){if(src->kind==PUMP_INGRESS_LOSS||(src->kind==PUMP_ENERGY&&(src->value.energy->kind!=SERVICE_ENERGY||src->value.energy->energy.gap)))r->unknown_loss++;return true;}
 r->staged=false;uint64_t first,end;unsigned stream;bool valid=true;PacketView v;
 if(src->kind==PUMP_PSD){stream=1;first=src->value.psd->first_raw;end=src->value.psd->end_raw;unsigned at=0;while(at<SUMMARY_RUNTIME_PSD&&r->psd[at].used)at++;if(at==SUMMARY_RUNTIME_PSD)return fail(r,STORE_EXHAUSTED)==STORE_OK;r->psd[at]=r->staged_psd;}
 else if(src->kind==PUMP_PCM){stream=2;if(!packet_decode(src->value.pcm,src->bytes,&v))return fail(r,STORE_INVALID)==STORE_OK;first=v.header.first_raw_sample_index;end=v.end_raw;valid=v.header.flags==LFN_VALID;if(valid&&!range_add(&r->pcm_valid,first,end))return fail(r,STORE_EXHAUSTED)==STORE_OK;}
 else {stream=0;first=src->value.energy->energy.record.first_raw;end=first+12288;}
 if(!range_add(&r->available,first,end))return fail(r,STORE_EXHAUSTED)==STORE_OK;
 r->seen[stream]=true;r->last_first[stream]=first;uint64_t safe=stream==0?first+6144:end;if(safe>r->frontier[stream])r->frontier[stream]=safe;
 r->source_hold=true;r->source_hold_first=stream==1?first+98304:first;return true;
}
static SummaryPending *pending(SummaryRuntime*r,unsigned file,StorageTicket ticket){for(unsigned k=0;k<STORAGE_RECEIPTS;k++)if(r->pending[file][k].used&&ticket_eq(r->pending[file][k].ticket,ticket))return &r->pending[file][k];return NULL;}
static bool admission(void*ctx,const StorageServiceSlot*slot,const StorageRequest*req){
 (void)req;SummaryFileObserver*f=ctx;SummaryRuntime*r=f->runtime;const RecordIdentity*i=&slot->identity;
 if(!receipt_role(f->index,i->type))return true;
 if(i->session_id!=r->session||slot->ticket.session_id!=r->session)return fail(r,STORE_INVALID)==STORE_OK;
 if(f->generation!=r->generation||f->file_id!=f->service->queue->file_id||pending(r,f->index,slot->ticket))return fail(r,STORE_INVALID)==STORE_OK;
 unsigned n=0;while(n<STORAGE_RECEIPTS&&r->pending[f->index][n].used)n++;if(n==STORAGE_RECEIPTS)return fail(r,STORE_EXHAUSTED)==STORE_OK;
 r->pending[f->index][n]=(SummaryPending){true,f->file_id,f->generation,slot->ticket,*i};
 if(!source_role(f->index,i->type))return true;
 if(!current(r,i))return fail(r,STORE_INVALID)==STORE_OK;
 if(i->type==RECORD_PSD){for(unsigned k=0;k<SUMMARY_RUNTIME_PSD;k++)if(r->psd[k].used&&r->psd[k].first==i->first_raw+98304&&r->psd[k].end==i->end_raw){if(r->psd[k].admitted)return fail(r,STORE_INVALID)==STORE_OK;r->psd[k].admitted=true;return true;}return fail(r,STORE_INVALID)==STORE_OK;}
 if(i->type==RECORD_MINUTE_SUMMARY){for(unsigned k=0;k<2;k++){SummaryOutput*o=&r->output[k];if(o->used&&o->submitted&&!o->admitted&&o->value.first_raw==i->first_raw&&o->value.end_raw==i->end_raw){o->admitted=true;o->receipt=slot->ticket;return true;}}return fail(r,STORE_INVALID)==STORE_OK;}
 return (i->flags&LFN_SATURATION)||contains(&r->pcm_valid,i->first_raw,i->end_raw)||fail(r,STORE_INVALID)==STORE_OK;
}
static bool terminal(void*ctx,const StorageServiceSlot*slot){
 SummaryFileObserver*f=ctx;SummaryRuntime*r=f->runtime;const RecordIdentity*i=&slot->identity;
 SummaryPending*p=pending(r,f->index,slot->ticket);
 if(!p&&!receipt_role(f->index,i->type))return true;
 if(!p||p->file_id!=f->file_id||p->generation!=r->generation||f->generation!=r->generation||!identity_eq(&p->identity,i))return fail(r,STORE_INVALID)==STORE_OK;
 if(slot->state!=STORE_SYNCED)return fail(r,STORE_INVALID)==STORE_OK;
 if(!source_role(f->index,i->type)){p->used=false;return true;}
 if(!current(r,i))return fail(r,STORE_INVALID)==STORE_OK;
 if(i->type==RECORD_PCM){if(!(i->flags&LFN_SATURATION)&&(!contains(&r->pcm_valid,i->first_raw,i->end_raw)||!range_add(&r->pcm_saved,i->first_raw,i->end_raw)))return fail(r,STORE_EXHAUSTED)==STORE_OK;}
 else if(i->type==RECORD_PSD){bool found=false;for(unsigned k=0;k<SUMMARY_RUNTIME_PSD;k++)if(r->psd[k].used&&r->psd[k].first==i->first_raw+98304&&r->psd[k].end==i->end_raw){r->psd[k].synced=true;found=true;}if(!found)return fail(r,STORE_INVALID)==STORE_OK;}
 else {bool found=false;for(unsigned k=0;k<2;k++)if(r->output[k].used&&r->output[k].admitted&&ticket_eq(r->output[k].receipt,slot->ticket)){r->output[k].synced=true;found=true;}if(!found)return fail(r,STORE_INVALID)==STORE_OK;}
 p->used=false;return true;
}
static bool any_pending(SummaryRuntime*r){for(unsigned f=0;f<4;f++)for(unsigned k=0;k<STORAGE_RECEIPTS;k++)if(r->pending[f][k].used)return true;return false;}
static bool source_pending(SummaryRuntime*r){for(unsigned f=0;f<2;f++)for(unsigned k=0;k<STORAGE_RECEIPTS;k++)if(r->pending[f][k].used)return true;return false;}
static StorageResult before_close(void*ctx){
 SummaryRuntime*r=ctx;if(r->error)return r->error;
 /* Keep pumps open while their source receipts reach terminal. Drained needs
  * sealing, which must follow the final derived SUMMARY submission. */
 for(unsigned k=0;k<4;k++)storage_writer_request_sync(r->pumps[k]->service->queue->writer);
 if(!r->eof){CaptureRouteFacts f;CaptureRouteHealth h;if(!capture_storage_route_facts_copy(r->route,&f)||!capture_storage_route_health(r->route,&h))return STORE_BUSY;
  if(!h.producer_closed||h.generation!=r->generation||f.generation!=r->generation||!f.final_clean||!f.final_capture_seq||!f.final_drain_seq||f.identity.session_id!=r->session)return fail(r,STORE_INVALID);
  if(source_pending(r))return STORE_BUSY;
  /* A completed MON capture remains retained across epochs. RAW has no
   * summary lifecycle; its clean current-generation proof may close while
   * preserving the session allocator and the prior completed capture. */
  if(!r->begun||r->closed){if(f.identity.config.mode)return STORE_OK;return fail(r,STORE_INVALID);}
  if(f.identity.capture_id!=r->identity.capture_id||f.identity.config.config_id!=r->identity.config_id||f.final_latest_raw<r->first_raw)return fail(r,STORE_INVALID);
  uint64_t samples=(f.final_latest_raw-r->first_raw)/48+1;if(samples>(UINT64_MAX-r->first_raw)/48)return fail(r,STORE_EXHAUSTED);
  r->final_end=r->first_raw+samples*48;if(r->final_end<r->cursor)return fail(r,STORE_INVALID);
  if(r->frontier[1]>r->final_end||r->frontier[2]>r->final_end||
     (r->seen[0]&&r->last_first[0]+12288>r->final_end))return fail(r,STORE_INVALID);
  r->complete_candidates=0;if(r->fft_first<=UINT64_MAX-UINT64_C(196560)&&f.final_latest_raw>=r->fft_first+196560)r->complete_candidates=1+(f.final_latest_raw-r->fft_first-196560)/98304;
  for(unsigned k=0;k<3;k++)r->frontier[k]=r->final_end;
  r->eof=true;
 }
 summary_runtime_step(r);return r->error?r->error:r->closed?STORE_OK:STORE_BUSY;
}
bool summary_runtime_init(SummaryRuntime*r,uint64_t session,uint64_t seed){if(!r||!session||!seed||seed==UINT64_MAX)return false;memset(r,0,sizeof(*r));r->session=session;r->next_id=seed;return true;}
bool summary_runtime_attach(void*ctx,StorageMultiSink*s,CaptureStorageRoute*route,StoragePump*const*p,unsigned n,uint64_t generation){
 SummaryRuntime*r=ctx;if(!r||!s||!route||!p||n!=4||!generation||s->session_id!=r->session||r->error||(r->begun&&!r->closed)||any_pending(r))return false;
 for(unsigned k=0;k<LFN_MARKER_CAPACITY;k++)if(r->markers[k].used){r->markers[k].used=false;r->marker_unlinked++;}
 r->sink=s;r->route=route;r->generation=generation;r->eof=false;
 for(unsigned k=0;k<n;k++){if(!p[k]||!p[k]->service||p[k]->service->queue->session_id!=r->session)return false;r->pumps[k]=p[k];r->files[k]=(SummaryFileObserver){r,p[k]->service,p[k]->service->queue->file_id,generation,k};p[k]->service->observer=(StorageServiceObserver){&r->files[k],admission,terminal};}
 s->observer=(StorageMultiObserver){r,preflight,commit,cancel,before_close};return true;
}
static bool output_busy(SummaryRuntime*r){for(unsigned k=0;k<2;k++)if(r->output[k].used)return true;return false;}
void summary_runtime_marker_synced(SummaryRuntime*r,const LfnMarker*m){
 if(!r||!m)return;
 if(m->start.session_id!=r->session||r->closed){r->marker_unlinked++;return;}
 for(unsigned k=0;k<LFN_MARKER_CAPACITY;k++)if(!r->markers[k].used){r->markers[k]=(SummaryMarker){m->marker_id,m->start.raw_index,m->start.capture_id,m->start.config_id,true};return;}
 r->marker_unlinked++;
}
static void marker_references(SummaryRuntime*r,MinuteSummary*m){
 for(unsigned k=0;k<LFN_MARKER_CAPACITY;k++){SummaryMarker*p=&r->markers[k];if(!p->used)continue;
  if(p->capture!=m->identity.capture_id||p->config!=m->identity.config_id)continue;
  if(p->raw<m->first_raw){p->used=false;r->marker_unlinked++;continue;}
  if(p->raw>=m->end_raw)continue;
  p->used=false;m->marker_total++;
  if(m->marker_count<SUMMARY_REFERENCES)m->markers[m->marker_count++]=p->id;else m->references_truncated=true;
 }
}
static void outputs(SummaryRuntime*r){for(unsigned k=0;k<2;k++){SummaryOutput*o=&r->output[k];if(!o->used)continue;
 if(!o->submitted){marker_references(r,&o->value);StorageResult e=storage_pump_summary(r->pumps[2],&o->value,&o->pump);if(e==STORE_OK)o->submitted=true;else if(e!=STORE_BUSY&&e!=STORE_CAPACITY)fail(r,e);}
 else if(!o->pump_done){StoragePumpResult p;StorageResult e=storage_pump_poll(r->pumps[2],o->pump,&p);if(e==STORE_OK&&p.done){if(p.aborted||p.last_submit.result!=STORE_OK){fail(r,STORE_INVALID);return;}if(storage_pump_ack(r->pumps[2],o->pump)==STORE_OK)o->pump_done=true;}else if(e!=STORE_OK&&e!=STORE_BUSY)fail(r,e);}
 if(o->pump_done&&o->synced){o->used=false;r->summary_count++;}
}}
void summary_runtime_step(SummaryRuntime*r){
 if(!r||r->error||!r->begun||r->closed)return;
 outputs(r);if(r->error)return;
 if(r->source_hold&&!r->sink->active)r->source_hold=false;
 for(unsigned steps=0;steps<16&&!r->error;steps++){
  if(r->core.ready){unsigned k=0;while(k<2&&r->output[k].used)k++;if(k==2)break;SummaryOutput*o=&r->output[k];memset(o,0,sizeof(*o));if(!minute_summary_take(&r->core,&o->value)){fail(r,STORE_INVALID);break;}o->used=true;if(o->value.summary_id==UINT64_MAX){fail(r,STORE_EXHAUSTED);break;}r->next_id=o->value.summary_id+1;continue;}
  if(r->core.have_input){minute_summary_step(&r->core);if(r->core.fault)fail(r,STORE_EXHAUSTED);continue;}
  uint64_t safe=r->frontier[0];for(unsigned k=1;k<3;k++)if(r->frontier[k]<safe)safe=r->frontier[k];
  if(r->source_hold&&r->source_hold_first<safe)safe=r->source_hold_first;
  for(unsigned f=0;f<4;f++)for(unsigned k=0;k<STORAGE_RECEIPTS;k++){SummaryPending*p=&r->pending[f][k];if(p->used&&source_role(f,p->identity.type)&&p->identity.type!=RECORD_MINUTE_SUMMARY){uint64_t first=p->identity.first_raw;if(p->identity.type==RECORD_PSD)first+=98304;if(first<safe)safe=first;}}
  if(safe<=r->cursor){if(r->eof&&r->cursor==r->final_end&&!any_pending(r)&&!r->source_hold){
    if(r->core.cursor!=r->core.current.observed_first_ns&&!r->core.closed){if(!minute_summary_flush(&r->core)){fail(r,STORE_INVALID);break;}continue;}
    if(!output_busy(r)){r->closed=true;r->closed_capture=r->identity.capture_id;}
   }break;}
  uint64_t end=safe;SummaryPsdFact*p=NULL;for(unsigned k=0;k<SUMMARY_RUNTIME_PSD;k++)if(r->psd[k].used){if(r->psd[k].first>r->cursor&&r->psd[k].first<end)end=r->psd[k].first;if(r->psd[k].first<=r->cursor&&r->psd[k].end>r->cursor){p=&r->psd[k];if(p->end<end)end=p->end;}}
  end=boundary(&r->pcm_valid,r->cursor,end);end=boundary(&r->pcm_saved,r->cursor,end);end=boundary(&r->available,r->cursor,end);
  uint64_t tail=UINT64_MAX,index=0;if(r->fft_first<=UINT64_MAX-98304){tail=r->fft_first+98304;if(r->cursor>=tail){index=(r->cursor-tail)/98304;uint64_t n=index+1;if(n<=(UINT64_MAX-tail)/98304){uint64_t next=tail+n*98304;if(next<end)end=next;}}else if(tail<end)end=tail;}
  SummaryInterval i={0};i.first_raw=r->cursor;i.end_raw=end;i.raw_known=i.requested=true;i.available=contains(&r->available,r->cursor,end);i.availability_known=i.available;i.pcm_valid=contains(&r->pcm_valid,r->cursor,end);i.pcm_saved=contains(&r->pcm_saved,r->cursor,end);i.statistic_known=p!=NULL;
  i.valid=p&&(p->flags&LFN_VALID)&&!(p->flags&LFN_SATURATION);i.saved=i.valid&&p->synced;if(p){i.flags=p->flags;memcpy(i.power,p->power,sizeof(i.power));}
  if(tail!=UINT64_MAX&&r->cursor>=tail&&(r->cursor-tail)%98304==0&&(!r->eof||index<r->complete_candidates)){i.expected_candidates=1;i.valid_candidates=i.valid?1:0;}
  if(!minute_summary_offer(&r->core,&i)){fail(r,STORE_INVALID);break;}r->cursor=end;minute_summary_step(&r->core);if(r->core.fault){fail(r,STORE_EXHAUSTED);break;}prune(r);
 }
 outputs(r);
}
