#include "storage_query.h"
#include <string.h>
enum { QS_FREE,QS_QUEUED,QS_ACTIVE,QS_CHUNK,QS_DONE };
enum { Q_IDLE,Q_OPEN,Q_READ,Q_RELEASE,Q_PUBLISH };
static bool lock(StorageQuery *q){return !q->locks.lock||q->locks.lock(q->locks.context);}
static void unlock(StorageQuery *q){if(q->locks.unlock)q->locks.unlock(q->locks.context);}
static StorageQuerySlot *slot(StorageQuery *q,StorageQueryTicket t){if(t.slot>=STORAGE_QUERY_SLOTS||!t.generation)return NULL;StorageQuerySlot *s=&q->slots[t.slot];return s->state!=QS_FREE&&s->generation==t.generation?s:NULL;}
static bool definition(RecordType t){return t==RECORD_SESSION||t==RECORD_CONFIG||t==RECORD_CAPTURE||t==RECORD_TIME_MAP||t==RECORD_CALIBRATION;}
bool storage_query_init(StorageQuery *q,StorageHistoryReader *r,const StorageQueueLocks *l,const StorageQueryFiles *f){
 if(!q||!r||!l||!!l->lock!=!!l->unlock||!f||!f->open||!f->release)return false;
 StorageQueueLocks locks=*l;StorageQueryFiles files=*f;memset(q,0,sizeof(*q));q->locks=locks;q->files=files;q->reader=r;q->next_order=1;q->active=STORAGE_QUERY_SLOTS;return true;
}
StorageResult storage_query_submit(StorageQuery *q,const StorageQueryRequest *request,StorageQueryTicket *out){
 const uint32_t supported=(1u<<RECORD_CONFIG)|(1u<<RECORD_CALIBRATION)|(1u<<RECORD_TIME_MAP)|(1u<<RECORD_PSD)|(1u<<RECORD_ENERGY)|(1u<<RECORD_PCM)|(1u<<RECORD_FAULT)|(1u<<RECORD_MINUTE_SUMMARY)|(1u<<RECORD_MARKER)|(1u<<RECORD_SESSION)|(1u<<RECORD_CAPTURE);
 if(!q||!request||!out||!request->session_id||!request->file_id||(request->end_sequence&&request->end_sequence<=request->first_sequence)||(request->type_mask&~supported))return STORE_INVALID;
 StorageQueryRequest copy=*request;if(!lock(q))return STORE_BUSY;
 if(q->next_order==UINT64_MAX){unlock(q);return STORE_EXHAUSTED;}
 for(unsigned k=0;k<STORAGE_QUERY_SLOTS;k++)if(q->slots[k].state==QS_FREE&&q->slots[k].generation!=UINT64_MAX){StorageQuerySlot *s=&q->slots[k];uint64_t generation=s->generation+1;memset(s,0,sizeof(*s));s->generation=generation;s->order=q->next_order++;s->request=copy;s->state=QS_QUEUED;*out=(StorageQueryTicket){generation,k};unlock(q);return STORE_OK;}
 unlock(q);return STORE_CAPACITY;
}
StorageResult storage_query_copy(StorageQuery *q,StorageQueryTicket t,StorageQueryChunk *out){if(!q||!out)return STORE_INVALID;if(!lock(q))return STORE_BUSY;StorageQuerySlot *s=slot(q,t);StorageResult r=!s?STORE_EXPIRED:s->state!=QS_CHUNK?STORE_BUSY:STORE_OK;if(r==STORE_OK)*out=s->chunk;unlock(q);return r;}
StorageResult storage_query_ack_chunk(StorageQuery *q,StorageQueryTicket t,uint64_t generation){if(!q||!generation)return STORE_INVALID;if(!lock(q))return STORE_BUSY;StorageQuerySlot *s=slot(q,t);StorageResult r=!s?STORE_EXPIRED:s->state==QS_CHUNK&&s->chunk.generation==generation?STORE_OK:STORE_BUSY;if(r==STORE_OK)s->state=QS_ACTIVE;unlock(q);return r;}
StorageResult storage_query_result(StorageQuery *q,StorageQueryTicket t,StorageQueryResult *out){if(!q||!out)return STORE_INVALID;if(!lock(q))return STORE_BUSY;StorageQuerySlot *s=slot(q,t);StorageResult r=!s?STORE_EXPIRED:s->state!=QS_DONE?STORE_BUSY:STORE_OK;if(r==STORE_OK)*out=s->result;unlock(q);return r;}
StorageResult storage_query_ack_result(StorageQuery *q,StorageQueryTicket t){if(!q)return STORE_INVALID;if(!lock(q))return STORE_BUSY;StorageQuerySlot *s=slot(q,t);StorageResult r=!s?STORE_EXPIRED:s->state!=QS_DONE?STORE_BUSY:STORE_OK;if(r==STORE_OK)s->state=QS_FREE;unlock(q);return r;}
StorageResult storage_query_cancel(StorageQuery *q,StorageQueryTicket t){if(!q)return STORE_INVALID;if(!lock(q))return STORE_BUSY;StorageQuerySlot *s=slot(q,t);StorageResult r=s?STORE_OK:STORE_EXPIRED;if(s)s->cancel=true;unlock(q);return r;}
static bool match(const StorageQueryRequest *q,const RecordIdentity *i){
 if(q->include_definitions&&definition(i->type))return true;
 return (!q->type_mask||(q->type_mask&(1u<<i->type)))&&i->record_seq>=q->first_sequence&&(!q->end_sequence||i->record_seq<q->end_sequence)&&(!q->capture_id||q->capture_id==i->capture_id)&&(!q->config_id||q->config_id==i->config_id);
}
static void complete(StorageQuery *q,StorageQueryStatus status){q->completion.status=status;if(q->have_source){q->completion.accepted_prefix=q->reader->prefix;q->completion.scanned_records=q->reader->records;q->phase=Q_RELEASE;}else q->phase=Q_PUBLISH;}
void storage_query_step(StorageQuery *q){
 if(!q)return;
 if(q->phase==Q_IDLE){
  if(!lock(q))return;
  unsigned selected=STORAGE_QUERY_SLOTS;uint64_t oldest=UINT64_MAX;for(unsigned k=0;k<STORAGE_QUERY_SLOTS;k++)if(q->slots[k].state==QS_QUEUED&&q->slots[k].order<oldest){selected=k;oldest=q->slots[k].order;}
  if(selected!=STORAGE_QUERY_SLOTS){q->active=selected;q->slots[selected].state=QS_ACTIVE;q->phase=Q_OPEN;q->have_source=false;q->exported=0;memset(&q->completion,0,sizeof(q->completion));q->completion.provenance=QUERY_HISTORICAL_CLAIM_UNVERIFIED;q->reader->prefix=0;q->reader->records=0;q->reader->state=HISTORY_CANCELLED;}unlock(q);return;
 }
 StorageQuerySlot *s=&q->slots[q->active];
 if(q->phase==Q_PUBLISH){if(!lock(q))return;
 s->result=q->completion;s->state=QS_DONE;q->phase=Q_IDLE;q->active=STORAGE_QUERY_SLOTS;unlock(q);return;}
 if(q->phase==Q_RELEASE){if(q->files.release(q->files.context,&q->source)!=STORE_IO_OK)q->completion.status=QUERY_IO;q->have_source=false;q->phase=Q_PUBLISH;return;}
 if(!lock(q))return;
 bool cancelled=s->cancel;unsigned state=s->state;unlock(q);
 if(cancelled){if(q->have_source)storage_history_cancel(q->reader);complete(q,QUERY_CANCELLED);return;}
 if(q->phase==Q_OPEN){
  memset(&q->source,0,sizeof(q->source));StorageQueryOpen opened=q->files.open(q->files.context,&s->request,&q->source);if(opened==QUERY_SOURCE_BUSY)return;
  if(opened!=QUERY_SOURCE_OK){complete(q,opened==QUERY_SOURCE_EXPIRED?QUERY_DATA_EXPIRED:opened==QUERY_SOURCE_MISSING?QUERY_NOT_FOUND:opened==QUERY_SOURCE_NOT_READY?QUERY_NOT_READY:opened==QUERY_SOURCE_DATA_MISSING?QUERY_DATA_MISSING:opened==QUERY_SOURCE_CORRUPT?QUERY_CORRUPT:QUERY_IO);return;}
  q->have_source=true;q->completion.provenance=q->source.provenance;q->completion.snapshot_bytes=q->source.bytes;
  if(q->source.session_id!=s->request.session_id||q->source.file_id!=s->request.file_id||q->source.provenance>QUERY_HISTORICAL_CLAIM_UNVERIFIED||!storage_history_init(q->reader,&q->source.backend,q->source.session_id,q->source.file_id,q->source.bytes,q->source.exact_size,q->source.check_crc,q->source.expected_crc)){complete(q,QUERY_IO);return;}q->phase=Q_READ;return;
 }
 if(q->phase!=Q_READ||state==QS_CHUNK)return;
 StorageHistoryReader *r=q->reader;
 if(r->state==HISTORY_RECORD){
  if(!match(&s->request,&r->view.identity)||q->exported==r->total){(void)storage_history_advance(r);q->exported=0;return;}
  if(!lock(q))return;
  if(s->cancel){unlock(q);return;}
  if(s->chunk.generation==UINT64_MAX){unlock(q);complete(q,QUERY_IO);return;}
  size_t n=r->total-q->exported;if(n>STORAGE_QUERY_CHUNK)n=STORAGE_QUERY_CHUNK;
  s->chunk.generation++;s->chunk.file_offset=r->offset;s->chunk.record_offset=q->exported;s->chunk.bytes=n;s->chunk.identity=r->view.identity;s->chunk.first=q->exported==0;s->chunk.last=q->exported+n==r->total;s->chunk.provenance=q->source.provenance;memcpy(s->chunk.data,r->record+q->exported,n);if(!q->exported)q->completion.exported_records++;q->exported+=n;s->state=QS_CHUNK;unlock(q);return;
 }
 if(r->state==HISTORY_SIZE||r->state==HISTORY_HEADER||r->state==HISTORY_BODY){storage_history_step(r);return;}
 complete(q,r->state==HISTORY_DONE?QUERY_COMPLETE:r->state==HISTORY_CAPACITY?QUERY_METADATA_CAPACITY:r->state==HISTORY_IO?QUERY_IO:r->state==HISTORY_CANCELLED?QUERY_CANCELLED:QUERY_CORRUPT);
}
