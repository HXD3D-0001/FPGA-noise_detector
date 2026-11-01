#include "storage_history.h"
#include "protocol.h"
#include <string.h>
static uint64_t get(const uint8_t *b,unsigned n){uint64_t v=0;for(unsigned k=0;k<n;k++)v|=(uint64_t)b[k]<<(8*k);return v;}
static bool definition(RecordType t){return t==RECORD_SESSION||t==RECORD_CONFIG||t==RECORD_CAPTURE||t==RECORD_TIME_MAP||t==RECORD_CALIBRATION;}
static void bad(StorageHistoryReader *r,StorageHistoryState state,StorageError e){r->state=state;r->error=e;}
bool storage_history_init(StorageHistoryReader *r,const StorageBackend *b,uint64_t session,uint64_t file,uint64_t limit,bool exact,bool check,uint32_t crc){
 if(!r||!b||!b->size||!b->read||!session||!file||(check&&!exact))return false;
 StorageBackend copy=*b;memset(r,0,sizeof(*r));r->backend=copy;r->session_id=session;r->file_id=file;r->limit=limit;r->exact_size=exact;r->check_crc=check;r->expected_crc=crc;r->state=HISTORY_SIZE;storage_registry_init(&r->registry);return true;
}
void storage_history_step(StorageHistoryReader *r){
 if(!r)return;
 if(r->state==HISTORY_SIZE){uint64_t n=0;if(r->backend.size(r->backend.context,&n)!=STORE_IO_OK){bad(r,HISTORY_IO,STORE_ERROR_IO);return;}if(n<r->limit||(r->exact_size&&n!=r->limit)){bad(r,HISTORY_TAIL,STORE_ERROR_DEPENDENCY);return;}r->state=HISTORY_HEADER;return;}
 size_t actual=0;
 if(r->state==HISTORY_HEADER){
  if(r->offset==r->limit){if(r->check_crc&&r->crc!=r->expected_crc)bad(r,HISTORY_TAIL,STORE_ERROR_DEPENDENCY);else r->state=HISTORY_DONE;return;}
  if(r->limit-r->offset<80){bad(r,HISTORY_TAIL,STORE_ERROR_DEPENDENCY);return;}
  if(r->backend.read(r->backend.context,r->offset,r->record,64,&actual)!=STORE_IO_OK||actual!=64){bad(r,HISTORY_IO,STORE_ERROR_SHORT_READ);return;}
  if(memcmp(r->record,"LFR1",4)||get(r->record+4,2)!=1||get(r->record+8,2)!=64||get(r->record+12,4)>RECORD_MAX_BYTES-80){bad(r,HISTORY_TAIL,STORE_ERROR_DEPENDENCY);return;}
  r->total=(size_t)get(r->record+12,4)+80;if(r->total>r->limit-r->offset){bad(r,HISTORY_TAIL,STORE_ERROR_DEPENDENCY);return;}r->used=64;r->state=HISTORY_BODY;return;
 }
 if(r->state!=HISTORY_BODY)return;
 size_t n=r->total-r->used;if(n>STORAGE_IO_CHUNK)n=STORAGE_IO_CHUNK;
 if(r->backend.read(r->backend.context,r->offset+r->used,r->record+r->used,n,&actual)!=STORE_IO_OK||actual!=n){bad(r,HISTORY_IO,STORE_ERROR_SHORT_READ);return;}r->used+=n;if(r->used!=r->total)return;
 if(!record_decode(r->record,r->total,&r->view)||r->view.identity.session_id!=r->session_id||r->view.identity.record_seq<=r->last_sequence||!storage_registry_validate(&r->registry,&r->view.identity,r->view.payload,r->view.payload_bytes)){bad(r,HISTORY_TAIL,STORE_ERROR_DEPENDENCY);return;}
 if(definition(r->view.identity.type)){StorageMetaResult result=storage_registry_apply(&r->registry,&r->view.identity,r->view.payload,r->view.payload_bytes,&r->scratch);if(result!=META_OK){bad(r,result==META_CAPACITY?HISTORY_CAPACITY:HISTORY_TAIL,STORE_ERROR_DEPENDENCY);return;}}
 r->crc=crc32c_extend(r->crc,r->record,r->total);r->last_sequence=r->view.identity.record_seq;r->records++;r->prefix=r->offset+r->total;r->state=HISTORY_RECORD;
}
bool storage_history_advance(StorageHistoryReader *r){if(!r||r->state!=HISTORY_RECORD)return false;r->offset=r->prefix;memset(&r->view,0,sizeof(r->view));r->state=HISTORY_HEADER;return true;}
void storage_history_cancel(StorageHistoryReader *r){if(r&&(r->state==HISTORY_SIZE||r->state==HISTORY_HEADER||r->state==HISTORY_BODY||r->state==HISTORY_RECORD)){r->state=HISTORY_CANCELLED;memset(&r->view,0,sizeof(r->view));}}
