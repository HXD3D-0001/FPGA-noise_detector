#include "storage_family.h"
#include "protocol.h"
#include <string.h>
static uint64_t get(const uint8_t *p,unsigned n){uint64_t v=0;for(unsigned i=0;i<n;i++)v|=(uint64_t)p[i]<<(8*i);return v;}
static void put(uint8_t *p,uint64_t v,unsigned n){for(unsigned i=0;i<n;i++)p[i]=(uint8_t)(v>>(8*i));}
static bool identity_equal(const RecordIdentity *a,const RecordIdentity *b){
 return a->type==b->type&&a->flags==b->flags&&a->config_id==b->config_id&&a->session_id==b->session_id&&
 a->capture_id==b->capture_id&&a->first_raw==b->first_raw&&a->end_raw==b->end_raw&&a->record_seq==b->record_seq;
}
static StorageError ioe(StorageIoResult v){return v==STORE_IO_NO_MEDIA?STORE_ERROR_NO_MEDIA:v==STORE_IO_NO_SPACE?STORE_ERROR_NO_SPACE:STORE_ERROR_IO;}
static void fail(StorageFamily *f,StorageError e){f->phase=FAMILY_FAULT;f->error=e;}
static void encode(uint8_t *b,const StorageFamilyEntry *e,uint64_t ledger,unsigned kind){
 memset(b,0,STORAGE_FAMILY_FRAME);memcpy(b,"SFL1",4);put(b+4,1,2);put(b+6,kind,2);
 put(b+8,e->identity.session_id,8);put(b+16,ledger,8);put(b+24,e->file_id,8);put(b+32,e->identity.record_seq,8);
 put(b+40,e->offset,8);put(b+48,e->total_bytes,8);put(b+56,e->identity.capture_id,8);
 put(b+64,e->identity.first_raw,8);put(b+72,e->identity.end_raw,8);put(b+80,e->identity.config_id,4);
 put(b+84,e->identity.type,2);put(b+86,e->identity.flags,2);put(b+88,e->record_crc,4);put(b+92,e->error,4);
 put(b+96,e->dependency_count,4);for(unsigned i=0;i<e->dependency_count;i++)put(b+104+8*i,e->dependencies[i],8);
 put(b+252,crc32c(b,252),4);
}
bool storage_family_frame_decode(const uint8_t *b,StorageFamilyEntry *e,uint64_t session,uint64_t ledger,unsigned *kind){
 if(!b||!e||!kind)return false;
 if(memcmp(b,"SFL1",4)||get(b+4,2)!=1||get(b+8,8)!=session||get(b+16,8)!=ledger||get(b+252,4)!=crc32c(b,252))return false;
 unsigned k=(unsigned)get(b+6,2),n=(unsigned)get(b+96,4);
 if(k<1||k>3||n>STORAGE_DEPENDENCIES||get(b+100,4))return false;
 for(unsigned i=104+8*n;i<252;i++)if(b[i])return false;
 memset(e,0,sizeof(*e));e->identity=(RecordIdentity){(RecordType)get(b+84,2),(uint16_t)get(b+86,2),(uint32_t)get(b+80,4),session,get(b+56,8),get(b+64,8),get(b+72,8),get(b+32,8)};
 e->file_id=get(b+24,8);e->offset=get(b+40,8);e->total_bytes=get(b+48,8);e->record_crc=(uint32_t)get(b+88,4);
 e->error=(StorageError)get(b+92,4);e->dependency_count=n;
 for(unsigned i=0;i<n;i++)e->dependencies[i]=get(b+104+8*i,8);
 if(!e->file_id||e->file_id==ledger||!e->identity.record_seq||e->identity.record_seq==UINT64_MAX||e->identity.type<RECORD_CONFIG||e->identity.type>RECORD_CAPTURE||
 e->identity.first_raw>e->identity.end_raw||e->total_bytes<80||e->total_bytes>RECORD_MAX_BYTES||get(b+92,4)>STORE_ERROR_DEPENDENCY)return false;
 if((k==1&&(e->offset!=UINT64_MAX||e->error!=STORE_ERROR_NONE))||
 (k==2&&(e->offset==UINT64_MAX||e->total_bytes>UINT64_MAX-e->offset||e->error!=STORE_ERROR_NONE))||
 (k==3&&e->error==STORE_ERROR_NONE))return false;
 *kind=k;return true;
}
bool storage_family_init(StorageFamily *f,const StorageBackend *b,uint64_t session,uint64_t ledger,uint64_t next){
 if(!f||!b||!b->size||!b->write||!b->sync||!session||!ledger||!next)return false;
 memset(f,0,sizeof(*f));f->ledger=*b;f->session_id=session;f->ledger_id=ledger;f->next_seq=next;f->phase=FAMILY_OPENING;return true;
}
bool storage_family_attach(StorageFamily *f,StorageWriter *w){
 if(!f||!w||f->phase!=FAMILY_READY||f->reserved_mode||f->count||f->total_records||f->file_count==STORAGE_FAMILY_FILES||w->session_id!=f->session_id||
 w->file_id==f->ledger_id||w->state!=SW_READY||w->prefix||w->file_bytes||w->next_seq>f->next_seq)return false;
 if(w->backend.context==f->ledger.context&&w->backend.write==f->ledger.write)return false;
 for(unsigned i=0;i<STORAGE_RECEIPTS;i++)if(w->slots[i].occupied)return false;
 for(unsigned i=0;i<f->file_count;i++)if(f->files[i]==w||f->files[i]->file_id==w->file_id||
  (f->files[i]->backend.context==w->backend.context&&f->files[i]->backend.write==w->backend.write))return false;
 f->files[f->file_count++]=w;return true;
}
StorageResult storage_family_query(const StorageFamily *f,uint64_t seq,StorageFamilyEntry *out){
 if(!f||!out||!seq)return STORE_INVALID;
 for(unsigned i=0;i<f->count;i++)if(f->entries[i].identity.record_seq==seq){*out=f->entries[i];return STORE_OK;}
 return STORE_EXPIRED;
}
static StorageSubmit submit_impl(StorageFamily *f,uint64_t file,const StorageRequest *r,const uint64_t *deps,unsigned n,bool reserved,uint64_t seq){
 StorageSubmit result={.result=STORE_INVALID};
 if(!f||!r||n>STORAGE_DEPENDENCIES||(n&&!deps)||f->reserved_mode!=reserved)return result;
 if(reserved&&(!seq||seq==UINT64_MAX||seq<f->next_seq))return result;
 if(f->phase!=FAMILY_READY){result.result=STORE_BUSY;return result;}
 unsigned k=0;for(;k<f->file_count;k++)if(f->files[k]->file_id==file)break;
 if(k==f->file_count||r->dependency_count||r->identity.record_seq||r->identity.session_id!=f->session_id||
 r->identity.type<RECORD_CONFIG||r->identity.type>RECORD_CAPTURE||r->identity.first_raw>r->identity.end_raw||
 r->payload_bytes>RECORD_MAX_BYTES-80||(!r->payload&&r->payload_bytes))return result;
 StorageWriter *w=f->files[k];
 if((f->closed_files&(1u<<k))||w->state!=SW_READY){result.result=STORE_BUSY;return result;}
 if(!w->backend.validate(w->backend.context,&r->identity,r->payload,r->payload_bytes))return result;
 for(unsigned i=0;i<n;i++){
  StorageFamilyEntry e;if(storage_family_query(f,deps[i],&e)!=STORE_OK||e.state!=FAMILY_SAVED){result.result=STORE_DEPENDENCY;return result;}
  for(unsigned j=0;j<i;j++)if(deps[i]==deps[j]){result.result=STORE_DEPENDENCY;return result;}
 }
 if(f->next_seq==UINT64_MAX){result.result=STORE_EXHAUSTED;return result;}
 if(f->count==STORAGE_FAMILY_ENTRIES){result.result=STORE_CAPACITY;return result;}
 if(f->ledger_bytes>UINT64_MAX-2*STORAGE_FAMILY_FRAME){result.result=STORE_EXHAUSTED;return result;}
 StorageFamilyEntry e={.identity=r->identity,.file_id=file,.offset=UINT64_MAX,.total_bytes=r->payload_bytes+80,.dependency_count=n,.state=FAMILY_RESERVED};
 e.identity.record_seq=reserved?seq:f->next_seq;
 size_t bytes;if(!record_encode(&e.identity,r->payload,r->payload_bytes,f->record,sizeof(f->record),&bytes))return result;
 e.record_crc=crc32c(f->record,bytes);for(unsigned i=0;i<n;i++)e.dependencies[i]=deps[i];
 f->entries[f->count++]=e;f->total_records++;f->active_file=k;f->have_ticket=false;f->next_seq=e.identity.record_seq+1;
 encode(f->frame,&e,f->ledger_id,1);f->phase=FAMILY_RESERVE_WRITE;
 result.result=STORE_OK;result.sequence_assigned=true;result.record_seq=e.identity.record_seq;return result;
}
StorageSubmit storage_family_submit(StorageFamily *f,uint64_t file,const StorageRequest *r,const uint64_t *deps,unsigned n){return submit_impl(f,file,r,deps,n,false,0);}
bool storage_family_use_reserved(StorageFamily *f){
 if(!f||f->phase!=FAMILY_READY||f->count||f->total_records||f->reserved_mode)return false;
 for(unsigned i=0;i<f->file_count;i++){
  StorageWriter *w=f->files[i];if(w->state!=SW_READY||w->prefix||w->file_bytes)return false;
  for(unsigned k=0;k<STORAGE_RECEIPTS;k++)if(w->slots[k].occupied)return false;
 }
 f->reserved_mode=true;return true;
}
StorageSubmit storage_family_append_reserved(StorageFamily *f,uint64_t file,const StorageRequest *r,const uint64_t *deps,unsigned n,uint64_t seq){return submit_impl(f,file,r,deps,n,true,seq);}
static void terminal(StorageFamily *f,StorageFamilyEntry *e,bool success,StorageError error){
 e->error=error;e->state=success?FAMILY_DATA_SYNCED:FAMILY_FAILED;
 encode(f->frame,e,f->ledger_id,success?2:3);f->phase=FAMILY_TERMINAL_WRITE;
}
void storage_family_step(StorageFamily *f,uint64_t now){
 if(!f||f->phase==FAMILY_FAULT)return;
 if(f->have_time&&now<f->last_now){fail(f,STORE_ERROR_CLOCK);return;}
 f->have_time=true;f->last_now=now;
 if(f->phase==FAMILY_OPENING){uint64_t size;StorageIoResult io=f->ledger.size(f->ledger.context,&size);
  if(io!=STORE_IO_OK)fail(f,ioe(io));else if(size)fail(f,STORE_ERROR_DEPENDENCY);else f->phase=FAMILY_READY;return;}
 if(f->phase==FAMILY_READY)return;
 StorageFamilyEntry *e=&f->entries[f->count-1];StorageWriter *w=f->files[f->active_file];
 switch(f->phase){
 case FAMILY_RESERVE_WRITE:case FAMILY_TERMINAL_WRITE:{
  size_t actual=0;StorageIoResult io=f->ledger.write(f->ledger.context,f->ledger_bytes,f->frame,sizeof(f->frame),&actual);
  if(io!=STORE_IO_OK)fail(f,ioe(io));else if(actual!=sizeof(f->frame))fail(f,STORE_ERROR_SHORT_WRITE);
  else {f->ledger_bytes+=sizeof(f->frame);f->phase=f->phase==FAMILY_RESERVE_WRITE?FAMILY_RESERVE_SYNC:FAMILY_TERMINAL_SYNC;}break;
 }
 case FAMILY_RESERVE_SYNC:case FAMILY_TERMINAL_SYNC:{
  StorageIoResult io=f->ledger.sync(f->ledger.context);if(io!=STORE_IO_OK){fail(f,ioe(io));break;}
  if(f->phase==FAMILY_RESERVE_SYNC){e->reservation_durable=true;f->phase=FAMILY_APPEND;}
  else {f->durable_bytes=f->ledger_bytes;e->terminal_durable=true;if(e->state==FAMILY_DATA_SYNCED)e->state=FAMILY_SAVED;
   if(f->have_ticket&&storage_writer_ack(w,f->active_ticket)!=STORE_OK){fail(f,STORE_ERROR_DEPENDENCY);break;}
   f->have_ticket=false;f->phase=FAMILY_READY;}break;
 }
 case FAMILY_APPEND:{
  RecordView v;if(!record_decode(f->record,(size_t)e->total_bytes,&v)){fail(f,STORE_ERROR_IO);break;}
  StorageRequest r={.identity=v.identity,.payload=v.payload,.payload_bytes=v.payload_bytes};r.identity.record_seq=0;
  StorageSubmit s=storage_writer_append_reserved(w,&r,e->identity.record_seq);
  if(s.result==STORE_OK){f->active_ticket=s.ticket;f->have_ticket=true;f->phase=FAMILY_DATA;}
  else if(s.result!=STORE_BUSY||w->state==SW_FAULT||w->state==SW_TAIL)terminal(f,e,false,STORE_ERROR_DEPENDENCY);
  break;
 }
 case FAMILY_DATA:{
  storage_writer_step(w,now);StorageReceipt r;
  if(storage_writer_poll(w,f->active_ticket,&r)!=STORE_OK||!identity_equal(&r.identity,&e->identity)||r.file_id!=e->file_id||r.total_bytes!=e->total_bytes){fail(f,STORE_ERROR_DEPENDENCY);break;}
  e->offset=r.offset;
  if(r.state==STORE_WRITTEN)storage_writer_request_sync(w);
  else if(r.state==STORE_SYNCED)terminal(f,e,true,STORE_ERROR_NONE);
  else if(r.state==STORE_FAILED)terminal(f,e,false,r.error);
  break;
 }
 default:break;
 }
}
bool storage_family_enable_indexed(StorageFamily *f){
 if(!f||f->phase!=FAMILY_READY||f->count||f->total_records||f->indexed||!f->ledger.read)return false;
 f->indexed=true;return true;
}
StorageResult storage_family_retire(StorageFamily *f,uint64_t seq){
 if(!f||!f->indexed||!seq)return STORE_INVALID;
 if(f->phase==FAMILY_FAULT)return STORE_BUSY;
 unsigned k=0;for(;k<f->count;k++)if(f->entries[k].identity.record_seq==seq)break;
 if(k==f->count)return STORE_EXPIRED;
 StorageFamilyEntry *e=&f->entries[k];
 if(!e->terminal_durable||(e->state!=FAMILY_SAVED&&e->state!=FAMILY_FAILED))return STORE_BUSY;
 if(e->identity.type!=RECORD_PCM&&e->identity.type!=RECORD_PSD&&e->identity.type!=RECORD_ENERGY&&e->identity.type!=RECORD_FAULT&&e->identity.type!=RECORD_MINUTE_SUMMARY&&e->identity.type!=RECORD_EVENT_META&&e->identity.type!=RECORD_MARKER)return STORE_DEPENDENCY;
 for(unsigned i=0;i<f->count;i++)if(i!=k)for(unsigned j=0;j<f->entries[i].dependency_count;j++)if(f->entries[i].dependencies[j]==seq)return STORE_BUSY;
 memmove(e,e+1,(f->count-k-1)*sizeof(*e));f->count--;f->retired_records++;return STORE_OK;
}
bool storage_family_replay_init(StorageFamilyReplay *r,const StorageBackend *b,uint64_t session,uint64_t ledger){
 if(!r||!b||!b->size||!b->read||!session||!ledger)return false;
 memset(r,0,sizeof(*r));r->ledger=*b;r->session_id=session;r->ledger_id=ledger;r->state=FAMILY_SCAN_SIZE;return true;
}
StorageResult storage_family_replay_query(const StorageFamilyReplay *r,uint64_t seq,StorageFamilyEntry *out){
 if(!r||!out||!seq)return STORE_INVALID;
 for(unsigned i=0;i<r->count;i++)if(r->entries[i].identity.record_seq==seq){*out=r->entries[i];return STORE_OK;}
 return STORE_EXPIRED;
}
void storage_family_replay_step(StorageFamilyReplay *r){
 if(!r)return;
 if(r->state==FAMILY_SCAN_SIZE){StorageIoResult io=r->ledger.size(r->ledger.context,&r->size);r->state=io==STORE_IO_OK?FAMILY_SCAN_FRAME:FAMILY_SCAN_IO;return;}
 if(r->state!=FAMILY_SCAN_FRAME)return;
 if(r->prefix==r->size){r->state=FAMILY_SCAN_DONE;return;}
 if(r->size-r->prefix<STORAGE_FAMILY_FRAME){r->state=FAMILY_SCAN_TAIL;return;}
 size_t actual=0;StorageIoResult io=r->ledger.read(r->ledger.context,r->prefix,r->frame,sizeof(r->frame),&actual);
 if(io!=STORE_IO_OK||actual!=sizeof(r->frame)){r->state=FAMILY_SCAN_IO;return;}
 StorageFamilyEntry e;unsigned kind;
 if(!storage_family_frame_decode(r->frame,&e,r->session_id,r->ledger_id,&kind)){r->state=FAMILY_SCAN_TAIL;return;}
 if(kind==1){
  if(e.identity.record_seq<=r->last_sequence||(r->count&&!r->entries[r->count-1].terminal_durable)){r->state=FAMILY_SCAN_TAIL;return;}
  if(r->count==STORAGE_FAMILY_ENTRIES){r->state=FAMILY_SCAN_CAPACITY;return;}
  for(unsigned i=0;i<e.dependency_count;i++){
   StorageFamilyEntry d;if(storage_family_replay_query(r,e.dependencies[i],&d)!=STORE_OK||!d.terminal_durable||
    (d.state!=FAMILY_REPLAY_UNVERIFIED&&d.state!=FAMILY_REPLAY_VERIFIED)){r->state=FAMILY_SCAN_TAIL;return;}
   for(unsigned j=0;j<i;j++)if(e.dependencies[i]==e.dependencies[j]){r->state=FAMILY_SCAN_TAIL;return;}
  }
  e.state=FAMILY_RESERVED;e.reservation_durable=true;r->entries[r->count++]=e;r->last_sequence=e.identity.record_seq;
 }else{
  if(!r->count){r->state=FAMILY_SCAN_TAIL;return;}
  StorageFamilyEntry *a=&r->entries[r->count-1];
  if(a->terminal_durable||!identity_equal(&a->identity,&e.identity)||a->file_id!=e.file_id||a->total_bytes!=e.total_bytes||
   a->record_crc!=e.record_crc||a->dependency_count!=e.dependency_count||memcmp(a->dependencies,e.dependencies,sizeof(e.dependencies))){r->state=FAMILY_SCAN_TAIL;return;}
  if(kind==2){
   uint64_t end=0;for(unsigned i=0;i+1<r->count;i++)if(r->entries[i].file_id==e.file_id&&
    (r->entries[i].state==FAMILY_REPLAY_UNVERIFIED||r->entries[i].state==FAMILY_REPLAY_VERIFIED))end=r->entries[i].offset+r->entries[i].total_bytes;
   if(e.offset!=end){r->state=FAMILY_SCAN_TAIL;return;}
  }
  e.state=kind==2?FAMILY_REPLAY_UNVERIFIED:FAMILY_FAILED;e.reservation_durable=e.terminal_durable=true;*a=e;
 }
 r->prefix+=STORAGE_FAMILY_FRAME;
}
StorageResult storage_family_replay_confirm(StorageFamilyReplay *r,uint64_t seq,uint64_t file,uint64_t offset,const uint8_t *bytes,size_t n,
 bool (*validate)(void *,const RecordIdentity *,const uint8_t *,size_t),void *context){
 if(!r||!bytes||!validate)return STORE_INVALID;
 unsigned k=0;for(;k<r->count;k++)if(r->entries[k].identity.record_seq==seq)break;
 if(k==r->count)return STORE_EXPIRED;
 StorageFamilyEntry *e=&r->entries[k];
 if(e->state!=FAMILY_REPLAY_UNVERIFIED&&e->state!=FAMILY_REPLAY_VERIFIED)return STORE_DEPENDENCY;
 for(unsigned i=0;i<e->dependency_count;i++){StorageFamilyEntry d;
  if(storage_family_replay_query(r,e->dependencies[i],&d)!=STORE_OK||d.state!=FAMILY_REPLAY_VERIFIED)return STORE_DEPENDENCY;}
 RecordView v;if(file!=e->file_id||offset!=e->offset||n!=e->total_bytes||crc32c(bytes,n)!=e->record_crc||!record_decode(bytes,n,&v)||
 !identity_equal(&v.identity,&e->identity)||!validate(context,&v.identity,v.payload,v.payload_bytes))return STORE_INVALID;
 e->state=FAMILY_REPLAY_VERIFIED;return STORE_OK;
}

StorageResult storage_family_freeze_file(StorageFamily *f,uint64_t file){
 if(!f||f->phase==FAMILY_FAULT)return STORE_INVALID;
 unsigned k=0;for(;k<f->file_count;k++)if(f->files[k]->file_id==file)break;
 if(k==f->file_count)return STORE_INVALID;
 if(f->phase!=FAMILY_READY&&f->active_file==k)return STORE_BUSY;
 StorageWriter *w=f->files[k];if(w->state!=SW_READY)return STORE_BUSY;
 for(unsigned i=0;i<STORAGE_RECEIPTS;i++)if(w->slots[i].occupied)return STORE_BUSY;
 f->closed_files|=(uint8_t)(1u<<k);return STORE_OK;
}
