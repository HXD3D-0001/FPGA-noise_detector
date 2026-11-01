#include "storage_family_index.h"
#include "protocol.h"
#include <string.h>
static bool same_identity(const RecordIdentity *a,const RecordIdentity *b){
 return a->session_id==b->session_id&&a->record_seq==b->record_seq&&a->type==b->type&&a->flags==b->flags&&
 a->capture_id==b->capture_id&&a->config_id==b->config_id&&a->first_raw==b->first_raw&&a->end_raw==b->end_raw;
}
bool storage_family_index_init(StorageFamilyIndex *q,const StorageBackend *b,uint64_t session,uint64_t ledger,uint64_t prefix,uint64_t sequence){
 if(!q||!b||!b->size||!b->read||!session||!ledger||!sequence||sequence==UINT64_MAX||prefix%(2*STORAGE_FAMILY_FRAME))return false;
 memset(q,0,sizeof(*q));q->ledger=*b;q->session_id=session;q->ledger_id=ledger;q->limit=prefix;q->target=sequence;q->state=FAMILY_INDEX_SIZE;return true;
}
static void decode_frame(StorageFamilyIndex *q){
 if(!q)return;
 if(q->state==FAMILY_INDEX_SIZE){uint64_t size=0;StorageIoResult io=q->ledger.size(q->ledger.context,&size);
  q->state=io!=STORE_IO_OK?FAMILY_INDEX_IO:size<q->limit?FAMILY_INDEX_TAIL:FAMILY_INDEX_RESERVE;return;}
 if(q->state!=FAMILY_INDEX_RESERVE&&q->state!=FAMILY_INDEX_TERMINAL)return;
 if(q->prefix==q->limit){q->state=FAMILY_INDEX_NOT_FOUND;return;}
 StorageFamilyEntry e;unsigned kind;
 if(!storage_family_frame_decode(q->frame+q->cache_used,&e,q->session_id,q->ledger_id,&kind)){q->state=FAMILY_INDEX_TAIL;return;}
 if(q->state==FAMILY_INDEX_RESERVE){
  if(kind!=1||e.identity.record_seq<=q->last_seq){q->state=FAMILY_INDEX_TAIL;return;}
  for(unsigned i=0;i<e.dependency_count;i++){
   if(!e.dependencies[i]||e.dependencies[i]>=e.identity.record_seq){q->state=FAMILY_INDEX_TAIL;return;}
   for(unsigned j=0;j<i;j++)if(e.dependencies[i]==e.dependencies[j]){q->state=FAMILY_INDEX_TAIL;return;}
  }
  unsigned k=0;for(;k<q->file_count;k++)if(q->files[k]==e.file_id)break;
  if(k==q->file_count){if(k==STORAGE_FAMILY_FILES){q->state=FAMILY_INDEX_TAIL;return;}q->files[q->file_count++]=e.file_id;}
  q->file=k;q->pending=e;q->state=FAMILY_INDEX_TERMINAL;
 }else{
  StorageFamilyEntry *p=&q->pending;
  if(kind==1||!same_identity(&p->identity,&e.identity)||p->file_id!=e.file_id||p->total_bytes!=e.total_bytes||p->record_crc!=e.record_crc||
   p->dependency_count!=e.dependency_count||memcmp(p->dependencies,e.dependencies,sizeof(e.dependencies))){q->state=FAMILY_INDEX_TAIL;return;}
  if(kind==2){if(e.offset!=q->ends[q->file]){q->state=FAMILY_INDEX_TAIL;return;}q->ends[q->file]=e.offset+e.total_bytes;}
  q->last_seq=e.identity.record_seq;q->state=FAMILY_INDEX_RESERVE;
  if(e.identity.record_seq==q->target){e.state=kind==2?FAMILY_REPLAY_UNVERIFIED:FAMILY_FAILED;e.reservation_durable=e.terminal_durable=true;q->match=e;q->state=FAMILY_INDEX_MATCH;}
 }
 q->prefix+=STORAGE_FAMILY_FRAME;
 q->cache_used+=STORAGE_FAMILY_FRAME;
}
void storage_family_index_step(StorageFamilyIndex *q){
 if(!q)return;
 if(q->state==FAMILY_INDEX_SIZE){decode_frame(q);return;}
 if(q->state!=FAMILY_INDEX_RESERVE&&q->state!=FAMILY_INDEX_TERMINAL)return;
 if(q->prefix==q->limit){q->state=FAMILY_INDEX_NOT_FOUND;return;}
 if(q->cache_used==q->cache_bytes){
  uint64_t remaining=q->limit-q->prefix;size_t n=remaining<sizeof(q->frame)?(size_t)remaining:sizeof(q->frame),actual=0;
  if(q->ledger.read(q->ledger.context,q->prefix,q->frame,n,&actual)!=STORE_IO_OK||actual!=n){q->state=FAMILY_INDEX_IO;return;}
  q->cache_used=0;q->cache_bytes=n;
 }
 /* At most sixteen frames and one <=4KiB read per scheduler call. */
 for(unsigned k=0;k<STORAGE_IO_CHUNK/STORAGE_FAMILY_FRAME&&q->cache_used<q->cache_bytes&&(q->state==FAMILY_INDEX_RESERVE||q->state==FAMILY_INDEX_TERMINAL);k++)decode_frame(q);
}
void storage_family_index_cancel(StorageFamilyIndex *q){if(q)q->state=FAMILY_INDEX_CANCELLED;}
StorageResult storage_family_index_result(const StorageFamilyIndex *q,StorageFamilyEntry *out){
 if(!q||!out)return STORE_INVALID;
 if(q->state==FAMILY_INDEX_MATCH){*out=q->match;return STORE_OK;}
 if(q->state==FAMILY_INDEX_NOT_FOUND)return STORE_EXPIRED;
 return q->state==FAMILY_INDEX_SIZE||q->state==FAMILY_INDEX_RESERVE||q->state==FAMILY_INDEX_TERMINAL?STORE_BUSY:STORE_INVALID;
}
StorageResult storage_family_index_check_record(const StorageFamilyIndex *q,uint64_t file,uint64_t offset,const uint8_t *b,size_t n,
 bool (*validate)(void *,const RecordIdentity *,const uint8_t *,size_t),void *context){
 if(!q||!b||!validate||q->state!=FAMILY_INDEX_MATCH)return STORE_INVALID;
 const StorageFamilyEntry *e=&q->match;if(e->state!=FAMILY_REPLAY_UNVERIFIED)return STORE_DEPENDENCY;
 RecordView v;if(file!=e->file_id||offset!=e->offset||n!=e->total_bytes||!record_decode(b,n,&v)||
 !same_identity(&v.identity,&e->identity)||crc32c(b,n)!=e->record_crc||!validate(context,&v.identity,v.payload,v.payload_bytes))return STORE_INVALID;
 return STORE_OK;
}

bool storage_family_index_init_observed(StorageFamilyIndex *q,const StorageBackend *b,uint64_t session,uint64_t ledger,uint64_t prefix,uint64_t sequence){if(!storage_family_index_init(q,b,session,ledger,prefix,sequence))return false;q->scope=FAMILY_INDEX_OBSERVED_HISTORY;return true;}
bool storage_family_index_next(StorageFamilyIndex *q,uint64_t sequence){
 if(!q||q->state!=FAMILY_INDEX_MATCH||!sequence||sequence==UINT64_MAX||sequence<=q->target||sequence<=q->last_seq)return false;
 q->target=sequence;q->state=FAMILY_INDEX_RESERVE;memset(&q->match,0,sizeof(q->match));return true;
}
bool storage_family_index_drain(StorageFamilyIndex *q){
 if(!q||(q->state!=FAMILY_INDEX_SIZE&&q->state!=FAMILY_INDEX_RESERVE&&q->state!=FAMILY_INDEX_TERMINAL&&q->state!=FAMILY_INDEX_MATCH))return false;
 q->target=UINT64_MAX;if(q->state==FAMILY_INDEX_MATCH)q->state=FAMILY_INDEX_RESERVE;memset(&q->match,0,sizeof(q->match));return true;
}
