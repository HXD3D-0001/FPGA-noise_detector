#include "storage_writer.h"
#include <string.h>
_Static_assert(sizeof(StorageWriter)-RECORD_MAX_BYTES<=32768,"Receipt core metadata exceeds reserved32KiB budget");
static uint32_t le32(const uint8_t*p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static StorageReceiptSlot *slot(StorageWriter*w,StorageTicket t){
 if(!w||t.session_id!=w->session_id||t.slot>=STORAGE_RECEIPTS)return NULL;
 StorageReceiptSlot*s=&w->slots[t.slot];return s->occupied&&s->generation==t.generation?s:NULL;
}
static void release_dependencies(StorageWriter*w,StorageReceiptSlot*s){
 for(unsigned i=0;i<s->dependency_count;i++){StorageReceiptSlot*d=slot(w,s->dependencies[i]);if(d&&d->references)d->references--;}
 s->dependency_count=0;
}
static void failure(StorageWriter*w,StorageError e){
 w->state=SW_FAULT;w->error=e;
 for(unsigned i=0;i<STORAGE_RECEIPTS;i++){
  StorageReceiptSlot*s=&w->slots[i];
  if(s->occupied&&(s->receipt.state==STORE_QUEUED||s->receipt.state==STORE_WRITTEN)){
   s->receipt.state=STORE_FAILED;s->receipt.error=e;release_dependencies(w,s);
  }
 }
}
static StorageError io_error(StorageIoResult r){
 return r==STORE_IO_NO_MEDIA?STORE_ERROR_NO_MEDIA:r==STORE_IO_NO_SPACE?STORE_ERROR_NO_SPACE:STORE_ERROR_IO;
}
static bool written(const StorageWriter*w){
 for(unsigned i=0;i<STORAGE_RECEIPTS;i++)if(w->slots[i].occupied&&w->slots[i].receipt.state==STORE_WRITTEN)return true;
 return false;
}
bool storage_writer_init(StorageWriter*w,const StorageBackend*b,uint64_t session,uint64_t file,uint64_t next,uint64_t interval){
 if(!w||!b||!b->size||!b->read||!b->write||!b->sync||!b->truncate||!b->validate||!session||!file||!next||!interval)return false;
 memset(w,0,sizeof(*w));w->backend=*b;w->session_id=session;w->file_id=file;w->next_seq=next;w->sync_interval_us=interval;w->state=SW_SCAN_SIZE;return true;
}
static StorageSubmit submit_impl(StorageWriter*w,const StorageRequest*r,bool reserved,uint64_t sequence){
 StorageSubmit result={.result=STORE_INVALID};
 if(!w||!r)return result;
 if(w->state!=SW_READY&&w->state!=SW_WRITE&&w->state!=SW_SYNC){result.result=STORE_BUSY;return result;}
 if(r->identity.session_id!=w->session_id||r->identity.record_seq||
 r->identity.type<RECORD_CONFIG||r->identity.type>RECORD_CAPTURE||r->identity.first_raw>r->identity.end_raw||
 (!r->payload&&r->payload_bytes)||r->payload_bytes>RECORD_MAX_BYTES-80||r->dependency_count>STORAGE_DEPENDENCIES||
 !w->backend.validate(w->backend.context,&r->identity,r->payload,r->payload_bytes))return result;
 for(unsigned i=0;i<r->dependency_count;i++){
  StorageReceiptSlot*d=slot(w,r->depends_on[i]);
  if(!d||d->receipt.state==STORE_FAILED||(d->receipt.state!=STORE_QUEUED&&(d->receipt.offset>w->prefix||d->receipt.total_bytes>w->prefix-d->receipt.offset))){result.result=STORE_DEPENDENCY;return result;}
  for(unsigned j=0;j<i;j++)if(r->depends_on[j].slot==r->depends_on[i].slot){result.result=STORE_DEPENDENCY;return result;}
 }
 unsigned index=STORAGE_RECEIPTS;
 for(unsigned i=0;i<STORAGE_RECEIPTS;i++)if(!w->slots[i].occupied&&w->slots[i].generation!=UINT64_MAX){index=i;break;}
 bool sync_due=written(w)&&(w->force_sync||(w->have_time&&w->last_now-w->last_sync_us>=w->sync_interval_us));
 bool full=w->state!=SW_READY||sync_due||index==STORAGE_RECEIPTS||r->payload_bytes+80>UINT64_MAX-w->prefix;
 if(reserved){
  if(!sequence||sequence==UINT64_MAX||sequence<w->next_seq)return result;
  if(full){result.result=STORE_BUSY;return result;}
  result.sequence_assigned=true;result.record_seq=sequence;w->next_seq=sequence+1;
 }else{
  if(w->next_seq==UINT64_MAX){result.result=STORE_EXHAUSTED;return result;}
  result.sequence_assigned=true;result.record_seq=w->next_seq++;
  if(full){
   if(!w->rejected_count)w->first_rejected_seq=result.record_seq;
   w->last_rejected_seq=result.record_seq;w->rejected_count++;result.result=STORE_CAPACITY;return result;
  }
 }
 StorageReceiptSlot*s=&w->slots[index];uint64_t generation=s->generation+1;
 RecordIdentity id=r->identity;id.record_seq=result.record_seq;size_t n=0;
 /* All syntactic fields preflighted above; failure would be an internal fault. */
 if(!record_encode(&id,r->payload,r->payload_bytes,w->buffer,sizeof(w->buffer),&n)){
  failure(w,STORE_ERROR_IO);result.result=STORE_INVALID;return result;
 }
 memset(s,0,sizeof(*s));s->occupied=true;s->generation=generation;
 result.ticket=(StorageTicket){w->session_id,generation,index};result.result=STORE_OK;
 s->receipt=(StorageReceipt){result.ticket,id,w->file_id,w->prefix,n,STORE_QUEUED,STORE_ERROR_NONE};
 s->dependency_count=r->dependency_count;
 for(unsigned i=0;i<r->dependency_count;i++){s->dependencies[i]=r->depends_on[i];slot(w,r->depends_on[i])->references++;}
 w->used=0;w->total=n;w->active=index;w->state=SW_WRITE;return result;
}
StorageSubmit storage_writer_submit(StorageWriter*w,const StorageRequest*r){return submit_impl(w,r,false,0);}
StorageSubmit storage_writer_append_reserved(StorageWriter*w,const StorageRequest*r,uint64_t seq){return submit_impl(w,r,true,seq);}
StorageResult storage_writer_poll(const StorageWriter*w,StorageTicket t,StorageReceipt*out){
 if(!w||!out)return STORE_INVALID;
 if(t.session_id!=w->session_id||t.slot>=STORAGE_RECEIPTS)return STORE_EXPIRED;
 const StorageReceiptSlot*s=&w->slots[t.slot];if(!s->occupied||s->generation!=t.generation)return STORE_EXPIRED;
 *out=s->receipt;return STORE_OK;
}
StorageResult storage_writer_ack(StorageWriter*w,StorageTicket t){
 StorageReceiptSlot*s=slot(w,t);if(!s)return STORE_EXPIRED;
 if(s->references||(s->receipt.state!=STORE_SYNCED&&s->receipt.state!=STORE_FAILED))return STORE_BUSY;
 s->occupied=false;return STORE_OK;
}
void storage_writer_request_sync(StorageWriter*w){if(w)w->force_sync=true;}
static void tail(StorageWriter*w,StorageTailReason reason){w->tail_reason=reason;w->state=SW_TAIL;}
static bool exact_read(StorageWriter*w,uint64_t off,uint8_t*b,size_t n){
 size_t actual=0;StorageIoResult io=w->backend.read(w->backend.context,off,b,n,&actual);
 if(io!=STORE_IO_OK){failure(w,io_error(io));return false;}
 if(actual!=n){failure(w,STORE_ERROR_SHORT_READ);return false;}return true;
}
static void scan_record(StorageWriter*w){
 RecordView view;
 if(!record_decode(w->buffer,w->total,&view)){tail(w,STORE_TAIL_CRC);return;}
 if(view.identity.session_id!=w->session_id){tail(w,STORE_TAIL_IDENTITY);return;}
 if(!view.identity.record_seq||view.identity.record_seq==UINT64_MAX||view.identity.record_seq<=w->scan_last_seq){tail(w,STORE_TAIL_ORDER);return;}
 if(!w->backend.validate(w->backend.context,&view.identity,view.payload,view.payload_bytes)){tail(w,STORE_TAIL_IDENTITY);return;}
 w->scan_last_seq=view.identity.record_seq;
 if(w->next_seq<=w->scan_last_seq)w->next_seq=w->scan_last_seq+1;
 w->prefix+=w->total;w->recovered_records++;w->state=SW_SCAN_HEADER;
}
void storage_writer_step(StorageWriter*w,uint64_t now){
 if(!w||w->state==SW_FAULT||w->state==SW_TAIL)return;
 if(w->have_time&&now<w->last_now){failure(w,STORE_ERROR_CLOCK);return;}
 w->last_now=now;if(!w->have_time){w->have_time=true;w->last_sync_us=now;}
 switch(w->state){
 case SW_SCAN_SIZE:{
  uint64_t size=0;StorageIoResult io=w->backend.size(w->backend.context,&size);
  if(io!=STORE_IO_OK){failure(w,io_error(io));break;}
  w->file_bytes=size;w->state=SW_SCAN_HEADER;break;
 }
 case SW_SCAN_HEADER:{
  if(w->prefix==w->file_bytes){w->state=SW_READY;break;}
  if(w->file_bytes-w->prefix<RECORD_HEADER_BYTES){tail(w,STORE_TAIL_PARTIAL);break;}
  if(!exact_read(w,w->prefix,w->buffer,RECORD_HEADER_BYTES))break;
  uint32_t payload=le32(w->buffer+12);
  if(memcmp(w->buffer,"LFR1",4)||w->buffer[4]!=1||w->buffer[5]||w->buffer[8]!=64||w->buffer[9]||payload>RECORD_MAX_BYTES-80){tail(w,STORE_TAIL_HEADER);break;}
  w->total=(size_t)payload+80;w->used=64;
  if(w->total>w->file_bytes-w->prefix){tail(w,STORE_TAIL_PARTIAL);break;}
  w->state=SW_SCAN_BODY;break;
 }
 case SW_SCAN_BODY:{
  size_t n=w->total-w->used;if(n>STORAGE_IO_CHUNK)n=STORAGE_IO_CHUNK;
  if(!exact_read(w,w->prefix+w->used,w->buffer+w->used,n))break;
  w->used+=n;if(w->used==w->total)scan_record(w);break;
 }
 case SW_WRITE:{
  StorageReceiptSlot*s=&w->slots[w->active];bool valid=true;
  for(unsigned i=0;i<s->dependency_count;i++){StorageReceiptSlot*d=slot(w,s->dependencies[i]);if(!d||d->receipt.state==STORE_FAILED)valid=false;}
  if(!valid){failure(w,STORE_ERROR_DEPENDENCY);break;}
  size_t n=w->total-w->used;if(n>STORAGE_IO_CHUNK)n=STORAGE_IO_CHUNK;
  size_t actual=0;StorageIoResult io=w->backend.write(w->backend.context,w->prefix+w->used,w->buffer+w->used,n,&actual);
  if(io!=STORE_IO_OK){failure(w,io_error(io));break;}
  if(actual!=n){failure(w,STORE_ERROR_SHORT_WRITE);break;}
  w->used+=n;if(w->used==w->total){w->prefix+=w->total;w->file_bytes=w->prefix;s->receipt.state=STORE_WRITTEN;w->state=SW_READY;}
  break;
 }
 case SW_READY:
  if(written(w)&&(w->force_sync||now-w->last_sync_us>=w->sync_interval_us))w->state=SW_SYNC;
  else if(!written(w))w->force_sync=false;
  break;
 case SW_SYNC:{
  StorageIoResult io=w->backend.sync(w->backend.context);if(io!=STORE_IO_OK){failure(w,io_error(io));break;}
  /* Confirm exact written slots, never all sequences below a watermark. */
  for(unsigned i=0;i<STORAGE_RECEIPTS;i++)if(w->slots[i].occupied&&w->slots[i].receipt.state==STORE_WRITTEN){w->slots[i].receipt.state=STORE_SYNCED;release_dependencies(w,&w->slots[i]);}
  w->last_sync_us=now;w->force_sync=false;w->state=SW_READY;break;
 }
 case SW_TRUNCATE:{
  StorageIoResult io=w->backend.truncate(w->backend.context,w->prefix);
  if(io!=STORE_IO_OK){failure(w,io_error(io));break;}w->state=SW_REPAIR_SYNC;break;
 }
 case SW_REPAIR_SYNC:{
  StorageIoResult io=w->backend.sync(w->backend.context);if(io!=STORE_IO_OK){failure(w,io_error(io));break;}
  w->file_bytes=w->prefix;w->last_sync_us=now;w->tail_reason=STORE_TAIL_NONE;w->state=SW_READY;break;
 }
 default:break;
 }
}
bool storage_writer_recover(StorageWriter*w){
 if(!w||(w->state!=SW_FAULT&&w->state!=SW_TAIL&&w->state!=SW_READY)||written(w))return false;
 w->state=SW_SCAN_SIZE;w->error=STORE_ERROR_NONE;w->tail_reason=STORE_TAIL_NONE;
 w->prefix=0;w->file_bytes=0;w->scan_last_seq=0;w->recovered_records=0;w->used=w->total=0;w->have_time=false;w->force_sync=false;return true;
}
bool storage_writer_repair_tail(StorageWriter*w){
 if(!w||w->state!=SW_TAIL)return false;
 w->state=SW_TRUNCATE;return true;
}
