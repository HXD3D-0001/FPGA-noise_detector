#include "storage_family_queue.h"
#include "storage_bootstrap.h"
#include <string.h>
static bool valid_ticket(StorageFamilyPort *p,StorageTicket t){return t.session_id==p->owner->family->session_id&&t.slot==p->file&&t.generation;}
static void step(void *v,uint64_t now){StorageFamilyPort *p=v;storage_family_step(p->owner->family,now);}
static StorageWriterState status(void *v,StorageError *error){
 StorageFamilyPort *p=v;StorageFamily *f=p->owner->family;StorageWriter *w=f->files[p->file];
 if(f->phase==FAMILY_FAULT){*error=f->error;return SW_FAULT;}
 if(w->state==SW_FAULT||w->state==SW_TAIL){*error=w->error?w->error:STORE_ERROR_DEPENDENCY;return SW_FAULT;}
 return f->phase==FAMILY_READY?SW_READY:SW_WRITE;
}
static StorageSubmit append(void *v,const StorageRequest *r,uint64_t seq){
 StorageFamilyPort *p=v;StorageFamilyQueue *o=p->owner;StorageFamily *f=o->family;StorageSubmit s={.result=STORE_BUSY};
 if(f->phase!=FAMILY_READY)return s;
 for(unsigned i=0;i<o->count;i++){
  uint64_t head=0;StorageResult result=storage_queue_head_sequence(o->ports[i].queue,&head);
  if(result==STORE_BUSY||result==STORE_INVALID||(result==STORE_OK&&head<seq))return s;
 }
 uint64_t deps[STORAGE_DEPENDENCIES];StorageRequest request=*r;request.dependency_count=0;
 for(unsigned i=0;i<r->dependency_count;i++)deps[i]=r->depends_on[i].generation;
 s=storage_family_append_reserved(f,p->queue->file_id,&request,deps,r->dependency_count,seq);
 if(s.result==STORE_OK)s.ticket=(StorageTicket){f->session_id,seq,p->file};
 return s;
}
static StorageResult poll(void *v,StorageTicket t,StorageReceipt *out){
 StorageFamilyPort *p=v;StorageFamily *f=p->owner->family;StorageFamilyEntry e;
 if(!out)return STORE_INVALID;
 if(!valid_ticket(p,t)||storage_family_query(f,t.generation,&e)!=STORE_OK||e.file_id!=p->queue->file_id)return STORE_EXPIRED;
 StorageReceiptState state=STORE_QUEUED;StorageError error=e.error;
 if(e.state==FAMILY_SAVED&&e.terminal_durable)state=STORE_SYNCED;
 else if(e.state==FAMILY_FAILED)state=STORE_FAILED;
 else if(e.state==FAMILY_DATA_SYNCED)state=STORE_WRITTEN;
 if(state!=STORE_SYNCED&&f->phase==FAMILY_FAULT){state=STORE_FAILED;error=f->error;}
 *out=(StorageReceipt){t,e.identity,e.file_id,e.offset,e.total_bytes,state,error};return STORE_OK;
}
static StorageResult ack(void *v,StorageTicket t){
 StorageFamilyPort *p=v;StorageFamily *f=p->owner->family;StorageFamilyEntry e;
 if(!valid_ticket(p,t)||storage_family_query(f,t.generation,&e)!=STORE_OK||e.file_id!=p->queue->file_id)return STORE_EXPIRED;
 if(!e.terminal_durable&&f->phase!=FAMILY_FAULT)return STORE_BUSY;
 if(p->owner->streaming&&e.terminal_durable&&f->phase!=FAMILY_FAULT&&
  (e.identity.type==RECORD_PCM||e.identity.type==RECORD_PSD||e.identity.type==RECORD_ENERGY||e.identity.type==RECORD_FAULT||e.identity.type==RECORD_MINUTE_SUMMARY||e.identity.type==RECORD_EVENT_META||e.identity.type==RECORD_MARKER)){
  StorageResult result=storage_family_retire(f,t.generation);if(result!=STORE_OK)return result;
  p->owner->sequence.admitted--;
 }
 return STORE_OK;
}
bool storage_family_queue_init(StorageFamilyQueue *o,StorageFamily *f,const StorageQueueLocks *locks){
 if(!o||!f||!locks||!!locks->lock!=!!locks->unlock||!f->file_count||!storage_family_use_reserved(f))return false;
 memset(o,0,sizeof(*o));o->family=f;o->locks=*locks;/* Keep STORAGE_BOOTSTRAP_DEFINITIONS admission slots reserved so an EVENT
   roll bootstrap (SESSION/CONFIG/CAPTURE) cannot be blocked by a full backlog. */
 _Static_assert((unsigned)STORAGE_FAMILY_DEFINITION_RESERVE<=(unsigned)STORAGE_BOOTSTRAP_DEFINITIONS,"reserve exceeds bootstrap cap");
 o->sequence=(StorageQueueSequence){.session_id=f->session_id,.next_seq=f->next_seq,.limit=STORAGE_FAMILY_ADMISSION_LIMIT};return true;
}
bool storage_family_queue_attach(StorageFamilyQueue *o,StorageQueue *q){
 if(!o||!o->family||!q||o->count==STORAGE_FAMILY_FILES||o->streaming||o->sequence.admitted||
 q->locks.context!=o->locks.context||q->locks.lock!=o->locks.lock||q->locks.unlock!=o->locks.unlock)return false;
 unsigned file=0;for(;file<o->family->file_count;file++)if(o->family->files[file]==q->writer)break;
 if(file==o->family->file_count)return false;
 uintptr_t begin=(uintptr_t)q->arena;if(begin>UINTPTR_MAX-STORAGE_POOL_BYTES)return false;
 for(unsigned i=0;i<o->count;i++){
  uintptr_t other=(uintptr_t)o->ports[i].queue->arena;
  if(o->ports[i].queue==q||o->ports[i].file==file||
   ((begin<other+STORAGE_POOL_BYTES&&other<begin+STORAGE_POOL_BYTES)&&
    !(q->pool&&q->pool==o->ports[i].queue->pool&&q->arena==o->ports[i].queue->arena)))return false;
 }
 StorageFamilyPort *p=&o->ports[o->count];memset(p,0,sizeof(*p));p->owner=o;p->queue=q;p->file=file;
 p->transport=(StorageQueueTransport){p,step,append,poll,ack,status};
 if(!storage_queue_bind_transport(q,&p->transport,&o->sequence))return false;
 o->count++;return true;
}

bool storage_family_queue_enable_streaming(StorageFamilyQueue *o){
 if(!o||!o->family||o->streaming||o->sequence.admitted||o->count!=o->family->file_count||!storage_family_enable_indexed(o->family))return false;
 o->streaming=true;o->sequence.rolling=true;return true;
}
StorageResult storage_family_queue_release_file(StorageFamilyQueue *o,unsigned port){
 if(!o||!o->streaming||port>=o->count)return STORE_INVALID;
 StorageFamily *f=o->family;StorageFamilyPort *p=&o->ports[port];StorageQueueHealth h;
 if(f->phase!=FAMILY_READY||!(f->closed_files&(1u<<p->file)))return STORE_BUSY;
 StorageResult r=storage_queue_health(p->queue,&h);if(r!=STORE_OK)return r;
 if(!h.input_closed||h.live_jobs||h.retained_receipts||p->queue->pending)return STORE_BUSY;
 uint64_t file=p->queue->file_id;unsigned removed=0;
 for(unsigned k=0;k<f->count;k++)if(f->entries[k].file_id==file){
  StorageFamilyEntry *e=&f->entries[k];if(!e->terminal_durable||e->state!=FAMILY_SAVED)return STORE_BUSY;
  for(unsigned j=0;j<f->count;j++)if(f->entries[j].file_id!=file)for(unsigned d=0;d<f->entries[j].dependency_count;d++)if(f->entries[j].dependencies[d]==e->identity.record_seq)return STORE_DEPENDENCY;
  removed++;
 }
 if(removed>o->sequence.admitted)return STORE_INVALID;
 unsigned dst=0;for(unsigned k=0;k<f->count;k++)if(f->entries[k].file_id!=file)f->entries[dst++]=f->entries[k];
 f->count=dst;f->retired_records+=removed;o->sequence.admitted-=removed;return STORE_OK;
}
