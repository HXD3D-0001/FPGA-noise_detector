#include "storage_queue.h"
#include <string.h>
enum { Q_FREE,Q_QUEUED,Q_DISPATCH,Q_OWNED,NO_RECEIPT=255 };
typedef struct {
 RecordIdentity identity;StorageTicket dependencies[STORAGE_DEPENDENCIES],writer_ticket;
 uint32_t payload_bytes;uint16_t block,blocks;uint8_t state,receipt,dependency_count;bool written_counted;
} QueueJob;
typedef struct {StorageReceipt value;uint64_t generation;uint16_t references;bool occupied;} QueueReceipt;
struct StorageQueueMeta {
 QueueJob jobs[STORAGE_QUEUE_JOBS];QueueReceipt receipts[STORAGE_RECEIPTS];
 uint8_t used[STORAGE_POOL_BYTES/STORAGE_BLOCK_BYTES],fifo[STORAGE_QUEUE_JOBS];
 unsigned head,count;StorageQueueHealth health;
};
_Static_assert(sizeof(StorageQueueMeta)<=STORAGE_STATUS_BLOCKS*STORAGE_BLOCK_BYTES,"Queue metadata exceeds32KiB reserve");
static bool lock(StorageQueue*q){return !q->locks.lock||q->locks.lock(q->locks.context);}
static void unlock(StorageQueue*q){if(q->locks.unlock)q->locks.unlock(q->locks.context);}
static QueueReceipt *receipt(StorageQueue*q,StorageTicket t){
 if(t.session_id!=q->session_id||t.slot>=STORAGE_RECEIPTS)return NULL;
 QueueReceipt*r=&q->meta->receipts[t.slot];return r->occupied&&r->generation==t.generation?r:NULL;
}
static bool control(RecordType t){return t==RECORD_SESSION||t==RECORD_CONFIG||t==RECORD_CALIBRATION||t==RECORD_CAPTURE||t==RECORD_TIME_MAP||t==RECORD_FAULT;}
static unsigned allocate(StorageQueue*q,unsigned blocks,bool ctl){
 uint8_t *used=q->pool?q->pool->used:q->meta->used;
 unsigned start=ctl?0:STORAGE_CONTROL_BLOCKS+STORAGE_STATUS_BLOCKS*(q->pool?STORAGE_SHARED_QUEUES:1),end=ctl?STORAGE_CONTROL_BLOCKS:STORAGE_POOL_BYTES/STORAGE_BLOCK_BYTES;
 for(unsigned first=start;first+blocks<=end;first++){
  unsigned i=0;while(i<blocks&&!used[first+i])i++;
  if(i==blocks){for(i=0;i<blocks;i++)used[first+i]=1;return first;}
 }
 return STORAGE_POOL_BYTES/STORAGE_BLOCK_BYTES;
}
static void free_payload(StorageQueue*q,QueueJob*j){
 uint8_t *used=q->pool?q->pool->used:q->meta->used;
 for(unsigned i=0;i<j->blocks;i++)used[j->block+i]=0;
 if(control(j->identity.type))q->meta->health.used_control_blocks-=j->blocks;else q->meta->health.used_data_blocks-=j->blocks;
 j->blocks=0;
}
static void dependencies_done(StorageQueue*q,QueueJob*j){
 for(unsigned i=0;i<j->dependency_count;i++){QueueReceipt*r=receipt(q,j->dependencies[i]);if(r&&r->references)r->references--;}
 j->dependency_count=0;
}
static void finish(StorageQueue*q,QueueJob*j,const StorageReceipt*value){
 if(j->receipt!=NO_RECEIPT){QueueReceipt*r=&q->meta->receipts[j->receipt];StorageTicket t=r->value.ticket;r->value=*value;r->value.ticket=t;}
 if(value->state==STORE_SYNCED)q->meta->health.synced++;else q->meta->health.failed++;
 dependencies_done(q,j);free_payload(q,j);j->state=Q_FREE;q->meta->health.live_jobs--;
}
static void fail_job(StorageQueue*q,QueueJob*j,StorageError error){
 StorageReceipt r={.identity=j->identity,.file_id=q->file_id,.offset=UINT64_MAX,.total_bytes=j->payload_bytes+80,.state=STORE_FAILED,.error=error};finish(q,j,&r);
}
bool storage_queue_init(StorageQueue*q,StorageWriter*w,uint8_t*arena,size_t n,const StorageQueueLocks*l){
 if(!q||!w||w->state!=SW_READY||!arena||n!=STORAGE_POOL_BYTES||((uintptr_t)arena&63)||!l||!!l->lock!=!!l->unlock)return false;
 for(unsigned i=0;i<STORAGE_RECEIPTS;i++)if(w->slots[i].occupied)return false;
 memset(q,0,sizeof(*q));q->writer=w;q->arena=arena;q->locks=*l;q->session_id=w->session_id;q->file_id=w->file_id;
 q->meta=(StorageQueueMeta*)(void*)(arena+STORAGE_CONTROL_BLOCKS*STORAGE_BLOCK_BYTES);
 memset(q->meta,0,sizeof(*q->meta));q->meta->health.ready=true;q->meta->health.next_seq=w->next_seq;
 q->validate=w->backend.validate;q->validate_context=w->backend.context;return true;
}
bool storage_queue_pool_init(StorageQueuePool *p,uint8_t *arena,size_t n,const StorageQueueLocks *l){
 if(!p||!arena||n!=STORAGE_POOL_BYTES||((uintptr_t)arena&63)||!l||!!l->lock!=!!l->unlock)return false;
 memset(p,0,sizeof(*p));p->arena=arena;p->locks=*l;return true;
}
bool storage_queue_init_shared(StorageQueue *q,StorageWriter *w,StorageQueuePool *p){
 if(!q||!w||!p||!p->arena||p->queues==STORAGE_SHARED_QUEUES||w->state!=SW_READY)return false;
 for(unsigned i=0;i<STORAGE_RECEIPTS;i++)if(w->slots[i].occupied)return false;
 for(unsigned i=0;i<p->queues;i++)if(p->members[i]==q||p->writers[i]==w)return false;
 memset(q,0,sizeof(*q));q->pool=p;q->writer=w;q->arena=p->arena;q->locks=p->locks;q->session_id=w->session_id;q->file_id=w->file_id;
 q->meta=(StorageQueueMeta*)(void*)(p->arena+(STORAGE_CONTROL_BLOCKS+p->queues*STORAGE_STATUS_BLOCKS)*STORAGE_BLOCK_BYTES);
 memset(q->meta,0,sizeof(*q->meta));q->meta->health.ready=false;q->meta->health.next_seq=w->next_seq;
 q->validate=w->backend.validate;q->validate_context=w->backend.context;p->members[p->queues]=q;p->writers[p->queues]=w;p->queues++;return true;
}
bool storage_queue_bind_transport(StorageQueue *q,const StorageQueueTransport *t,StorageQueueSequence *s){
 if(!q||!q->meta||!t||!t->step||!t->append||!t->poll||!t->ack||!t->status||!s||s->session_id!=q->session_id||
 !s->next_seq||!s->limit||q->transport||q->writer->state!=SW_READY||q->pending||q->meta->health.live_jobs||q->meta->health.accepted||q->meta->health.rejected||q->meta->health.retained_receipts||q->writer->prefix)return false;
 for(unsigned i=0;i<STORAGE_RECEIPTS;i++)if(q->writer->slots[i].occupied)return false;
 q->transport=t;q->sequence=s;q->meta->health.ready=true;return true;
}
StorageResult storage_queue_rebind_empty(StorageQueue *q,StorageWriter *w){
 if(!q||!w||!q->pool||!q->sequence||!q->transport||w!=q->writer||w->state!=SW_READY||w->session_id!=q->session_id)return STORE_INVALID;
 if(!lock(q))return STORE_BUSY;
 StorageQueueMeta *m=q->meta;
 if(!m->health.input_closed||m->health.live_jobs||m->health.retained_receipts||q->pending||m->count){unlock(q);return STORE_BUSY;}
 for(unsigned k=0;k<STORAGE_RECEIPTS;k++)if(m->receipts[k].occupied||m->receipts[k].references){unlock(q);return STORE_BUSY;}
 memset(&m->health,0,sizeof(m->health));m->health.ready=true;m->health.input_paused=true;m->health.next_seq=q->sequence->next_seq;
 q->file_id=w->file_id;q->validate=w->backend.validate;q->validate_context=w->backend.context;unlock(q);return STORE_OK;
}
StorageResult storage_queue_head_sequence(StorageQueue *q,uint64_t *out){
 if(!q||!q->meta||!out)return STORE_INVALID;
 if(!lock(q))return STORE_BUSY;
 StorageResult r=STORE_EXPIRED;if(q->meta->count){*out=q->meta->jobs[q->meta->fifo[q->meta->head]].identity.record_seq;r=STORE_OK;}
 unlock(q);return r;
}
StorageSubmit storage_queue_submit_reserved(StorageQueue*q,const StorageQueueRequest*request,bool reserve){
 StorageSubmit result={.result=STORE_INVALID};if(!q||!q->meta||!request)return result;
 if(q->pool&&!q->transport){result.result=STORE_BUSY;return result;}
 const StorageRequest*r=&request->record;
 if(r->identity.session_id!=q->session_id||r->identity.record_seq||r->identity.type<RECORD_CONFIG||r->identity.type>RECORD_CAPTURE||
 r->identity.first_raw>r->identity.end_raw||(!r->payload&&r->payload_bytes)||r->payload_bytes>RECORD_MAX_BYTES-80||r->dependency_count>STORAGE_DEPENDENCIES||
 !q->validate(q->validate_context,&r->identity,r->payload,r->payload_bytes))return result;
 if(!lock(q)){result.result=STORE_BUSY;return result;}
 StorageQueueMeta*m=q->meta;
 if(!m->health.ready||m->health.input_paused||m->health.input_closed){result.result=STORE_BUSY;unlock(q);return result;}
 unsigned metadata=0;
 for(unsigned i=0;i<r->dependency_count;i++){
  QueueReceipt*d=receipt(q,r->depends_on[i]);bool duplicate=false;
  for(unsigned j=0;j<i;j++)if(r->depends_on[i].slot==r->depends_on[j].slot)duplicate=true;
  if(!d||d->value.state==STORE_FAILED||duplicate){result.result=STORE_DEPENDENCY;unlock(q);return result;}
  const RecordIdentity*id=&d->value.identity;
  if(q->sequence&&q->sequence->rolling&&(id->type==RECORD_PCM||id->type==RECORD_PSD||id->type==RECORD_ENERGY||id->type==RECORD_FAULT||id->type==RECORD_MINUTE_SUMMARY||id->type==RECORD_EVENT_META||id->type==RECORD_MARKER)){
   result.result=STORE_DEPENDENCY;unlock(q);return result;
  }
  if(id->type==RECORD_SESSION)metadata|=1;
  if(id->type==RECORD_CONFIG&&id->config_id==r->identity.config_id)metadata|=2;
  if(id->type==RECORD_CAPTURE&&id->capture_id==r->identity.capture_id&&id->config_id==r->identity.config_id)metadata|=4;
 }
 if((r->identity.type==RECORD_PCM||r->identity.type==RECORD_PSD||r->identity.type==RECORD_ENERGY||r->identity.type==RECORD_MINUTE_SUMMARY||r->identity.type==RECORD_MARKER)&&metadata!=7){result.result=STORE_DEPENDENCY;unlock(q);return result;}
 uint64_t next=q->sequence?q->sequence->next_seq:m->health.next_seq;
 if(next==UINT64_MAX){result.result=STORE_EXHAUSTED;unlock(q);return result;}
 result.sequence_assigned=true;result.record_seq=next;
 if(q->sequence)q->sequence->next_seq++;else m->health.next_seq++;
 if(q->sequence&&q->sequence->admitted>=q->sequence->limit&&!reserve){m->health.rejected++;result.result=STORE_CAPACITY;unlock(q);return result;}
 unsigned ji=STORAGE_QUEUE_JOBS,ri=NO_RECEIPT;
 for(unsigned i=control(r->identity.type)?0:STORAGE_CONTROL_JOBS;i<STORAGE_QUEUE_JOBS;i++)if(m->jobs[i].state==Q_FREE){ji=i;break;}
 if(request->receipt_required)for(unsigned i=control(r->identity.type)?0:STORAGE_CONTROL_JOBS;i<STORAGE_RECEIPTS;i++)if(!m->receipts[i].occupied&&m->receipts[i].generation!=UINT64_MAX){ri=i;break;}
 unsigned blocks=(unsigned)((r->payload_bytes+STORAGE_BLOCK_BYTES-1)/STORAGE_BLOCK_BYTES);
 unsigned first=STORAGE_POOL_BYTES/STORAGE_BLOCK_BYTES;
 if(ji!=STORAGE_QUEUE_JOBS&&(!request->receipt_required||ri!=NO_RECEIPT))first=allocate(q,blocks,control(r->identity.type));
 if(first==STORAGE_POOL_BYTES/STORAGE_BLOCK_BYTES){m->health.rejected++;result.result=STORE_CAPACITY;unlock(q);return result;}
 QueueJob*j=&m->jobs[ji];memset(j,0,sizeof(*j));j->identity=r->identity;j->identity.record_seq=result.record_seq;
 j->payload_bytes=(uint32_t)r->payload_bytes;j->block=(uint16_t)first;j->blocks=(uint16_t)blocks;j->state=Q_QUEUED;j->receipt=(uint8_t)ri;j->dependency_count=(uint8_t)r->dependency_count;
 if(r->payload_bytes)memcpy(q->arena+first*STORAGE_BLOCK_BYTES,r->payload,r->payload_bytes);
 for(unsigned i=0;i<r->dependency_count;i++){j->dependencies[i]=r->depends_on[i];receipt(q,r->depends_on[i])->references++;}
 if(request->receipt_required){
  QueueReceipt*c=&m->receipts[ri];if(q->sequence)c->generation=result.record_seq;else c->generation++;c->occupied=true;c->references=0;
  result.ticket=(StorageTicket){q->session_id,c->generation,ri};
  c->value=(StorageReceipt){result.ticket,j->identity,q->file_id,UINT64_MAX,r->payload_bytes+80,STORE_QUEUED,STORE_ERROR_NONE};m->health.retained_receipts++;
 }
 m->fifo[(m->head+m->count)%STORAGE_QUEUE_JOBS]=(uint8_t)ji;m->count++;m->health.queued=m->count;m->health.accepted++;m->health.live_jobs++;
 if(control(j->identity.type))m->health.used_control_blocks+=blocks;else m->health.used_data_blocks+=blocks;
 if(q->sequence)q->sequence->admitted++;
 result.result=STORE_OK;unlock(q);return result;
}
StorageSubmit storage_queue_submit(StorageQueue*q,const StorageQueueRequest*request){return storage_queue_submit_reserved(q,request,false);}
StorageResult storage_queue_poll(StorageQueue*q,StorageTicket t,StorageReceipt*out){
 if(!q||!q->meta||!out)return STORE_INVALID;
 if(!lock(q))return STORE_BUSY;
 QueueReceipt*r=receipt(q,t);StorageResult result=r?STORE_OK:STORE_EXPIRED;if(r)*out=r->value;unlock(q);return result;
}
StorageResult storage_queue_ack(StorageQueue*q,StorageTicket t){
 if(!q||!q->meta)return STORE_INVALID;
 if(!lock(q))return STORE_BUSY;
 QueueReceipt*r=receipt(q,t);StorageResult result=STORE_EXPIRED;
 if(r){result=STORE_BUSY;if(!r->references&&(r->value.state==STORE_SYNCED||r->value.state==STORE_FAILED)){r->occupied=false;q->meta->health.retained_receipts--;result=STORE_OK;}}
 unlock(q);return result;
}
StorageResult storage_queue_health(StorageQueue*q,StorageQueueHealth*out){
 if(!q||!q->meta||!out)return STORE_INVALID;
 if(!lock(q))return STORE_BUSY;
 *out=q->meta->health;if(q->sequence)out->next_seq=q->sequence->next_seq;unlock(q);return STORE_OK;
}
static void pop(StorageQueue*q){StorageQueueMeta*m=q->meta;m->head=(m->head+1)%STORAGE_QUEUE_JOBS;m->count--;m->health.queued=m->count;}
static bool finalize(StorageQueue*q){
 if(!q->pending)return true;
 if(!lock(q))return false;
 QueueJob*j=&q->meta->jobs[q->pending_job];StorageSubmit*r=&q->pending_result;
 if(r->result==STORE_OK){j->writer_ticket=r->ticket;j->state=Q_OWNED;free_payload(q,j);pop(q);}
 else if(r->result==STORE_BUSY)j->state=Q_QUEUED;
 else{fail_job(q,j,r->result==STORE_DEPENDENCY?STORE_ERROR_DEPENDENCY:STORE_ERROR_IO);pop(q);}
 q->pending=false;unlock(q);return true;
}
void storage_queue_step(StorageQueue*q,uint64_t now){
 if(!q||!q->meta||(q->pool&&!q->transport)||!finalize(q))return;
 StorageWriter*w=q->writer;
 if(q->transport)q->transport->step(q->transport->context,now);else storage_writer_step(w,now); /* Never hold queue lock over IO. */
 StorageError error=w->error;StorageWriterState state=q->transport?q->transport->status(q->transport->context,&error):w->state;
 if(!lock(q))return;
 StorageQueueMeta*m=q->meta;
 m->health.ready=state==SW_READY||state==SW_WRITE||state==SW_SYNC;
 if(!q->sequence&&m->health.next_seq<w->next_seq)m->health.next_seq=w->next_seq;
 for(unsigned i=0;i<STORAGE_QUEUE_JOBS;i++){
  QueueJob*j=&m->jobs[i];if(j->state!=Q_OWNED)continue;
  StorageReceipt r;
  if((q->transport?q->transport->poll(q->transport->context,j->writer_ticket,&r):storage_writer_poll(w,j->writer_ticket,&r))!=STORE_OK){fail_job(q,j,STORE_ERROR_IO);continue;}
  if((r.state==STORE_WRITTEN||r.state==STORE_SYNCED)&&!j->written_counted){m->health.written++;j->written_counted=true;}
  if(r.state==STORE_WRITTEN&&j->receipt!=NO_RECEIPT){QueueReceipt*c=&m->receipts[j->receipt];StorageTicket t=c->value.ticket;c->value=r;c->value.ticket=t;}
  if(r.state==STORE_SYNCED||r.state==STORE_FAILED){
   if((q->transport?q->transport->ack(q->transport->context,j->writer_ticket):storage_writer_ack(w,j->writer_ticket))==STORE_OK)finish(q,j,&r);
  }
 }
 if(state==SW_FAULT){while(m->count){QueueJob*j=&m->jobs[m->fifo[m->head]];fail_job(q,j,error);pop(q);}unlock(q);return;}
 if(!m->count||state!=SW_READY){unlock(q);return;}
 unsigned ji=m->fifo[m->head];QueueJob*j=&m->jobs[ji];StorageRequest r={0};r.identity=j->identity;r.identity.record_seq=0;r.payload=q->arena+j->block*STORAGE_BLOCK_BYTES;r.payload_bytes=j->payload_bytes;
 bool invalid=false;
 for(unsigned i=0;i<j->dependency_count;i++){
  QueueReceipt*d=receipt(q,j->dependencies[i]);
  if(q->transport){
   if(!d||d->value.state==STORE_FAILED){invalid=true;break;}
   if(d->value.state!=STORE_SYNCED){unlock(q);return;}
   r.depends_on[r.dependency_count++]=(StorageTicket){q->session_id,d->value.identity.record_seq,0};continue;
  }
  if(!d||d->value.state==STORE_FAILED||d->value.state==STORE_QUEUED){invalid=true;break;}
  if(d->value.state==STORE_SYNCED){if(d->value.offset>w->prefix||d->value.total_bytes>w->prefix-d->value.offset)invalid=true;continue;}
  bool found=false;
  for(unsigned k=0;k<STORAGE_QUEUE_JOBS;k++)if(m->jobs[k].state==Q_OWNED&&m->jobs[k].receipt==j->dependencies[i].slot){r.depends_on[r.dependency_count++]=m->jobs[k].writer_ticket;found=true;break;}
  if(!found)invalid=true;
 }
 if(invalid){fail_job(q,j,STORE_ERROR_DEPENDENCY);pop(q);unlock(q);return;}
 j->state=Q_DISPATCH;uint64_t seq=j->identity.record_seq;unlock(q);
 q->pending_result=q->transport?q->transport->append(q->transport->context,&r,seq):storage_writer_append_reserved(w,&r,seq);q->pending_job=ji;q->pending=true;(void)finalize(q);
}
bool storage_queue_recover(StorageQueue*q){
 if(!q||!q->meta||q->transport||q->pending||!lock(q))return false;
 bool idle=q->meta->health.live_jobs==0;if(idle)q->meta->health.ready=false;unlock(q);
 return idle&&storage_writer_recover(q->writer);
}
bool storage_queue_repair_tail(StorageQueue*q){
 if(!q||!q->meta||q->transport||q->pending||!lock(q))return false;
 bool idle=q->meta->health.live_jobs==0;if(idle)q->meta->health.ready=false;unlock(q);
 return idle&&storage_writer_repair_tail(q->writer);
}

StorageResult storage_queue_pause_pristine(StorageQueue **qs,unsigned count){
 if(!qs||!count||count>STORAGE_SHARED_QUEUES)return STORE_INVALID;
 for(unsigned i=0;i<count;i++){
  if(!qs[i]||!qs[i]->meta)return STORE_INVALID;
  if(qs[i]->locks.context!=qs[0]->locks.context||qs[i]->locks.lock!=qs[0]->locks.lock||qs[i]->locks.unlock!=qs[0]->locks.unlock)return STORE_INVALID;
  for(unsigned j=0;j<i;j++)if(qs[j]==qs[i])return STORE_INVALID;
 }
 if(!lock(qs[0]))return STORE_BUSY;
 StorageResult result=STORE_OK;
 for(unsigned i=0;i<count;i++){
  StorageQueueHealth *h=&qs[i]->meta->health;
  if(!h->ready||h->accepted||h->rejected||h->live_jobs||h->retained_receipts||h->input_paused||h->input_closed||qs[i]->pending){result=STORE_BUSY;break;}
 }
 if(result==STORE_OK)for(unsigned i=0;i<count;i++)qs[i]->meta->health.input_paused=true;
 unlock(qs[0]);return result;
}
StorageResult storage_queue_resume_pristine(StorageQueue *q){
 if(!q||!q->meta)return STORE_INVALID;
 if(!lock(q))return STORE_BUSY;
 StorageQueueHealth *h=&q->meta->health;StorageResult r=STORE_BUSY;
 if(h->ready&&h->input_paused&&!h->input_closed&&!h->live_jobs&&!q->pending){h->input_paused=false;r=STORE_OK;}
 unlock(q);return r;
}
StorageResult storage_queue_close_empty(StorageQueue *q,StorageQueueHealth *out){
 if(!q||!q->meta||!out)return STORE_INVALID;
 if(!lock(q))return STORE_BUSY;
 StorageQueueHealth *h=&q->meta->health;StorageResult r=STORE_BUSY;
 if(h->ready&&!h->input_paused&&!h->live_jobs&&!q->pending){
  h->input_closed=true;*out=*h;if(q->sequence)out->next_seq=q->sequence->next_seq;r=STORE_OK;
 }
 unlock(q);return r;
}
