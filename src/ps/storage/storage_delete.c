#include "storage_delete.h"
#include "protocol.h"
#include <string.h>
static void put(uint8_t *b,uint64_t n,unsigned len){for(unsigned k=0;k<len;k++)b[k]=(uint8_t)(n>>(8*k));}
static uint64_t get(const uint8_t *b,unsigned len){uint64_t n=0;for(unsigned k=0;k<len;k++)n|=(uint64_t)b[k]<<(8*k);return n;}
static bool name_ok(const char *name){size_t n=0;for(;n<64&&name[n];n++){char c=name[n];if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.'))return false;}return n&&n<64&&strcmp(name,".")&&strcmp(name,"..");}
static bool eligible(const StorageRetentionFile *f){return f&&f->session_id&&f->file_id&&f->catalog_id&&f->ledger_id&&f->file_id!=f->catalog_id&&f->file_id!=f->ledger_id&&f->catalog_id!=f->ledger_id&&(unsigned)f->family<RETAIN_FAMILIES&&f->closed&&f->seal_verified&&!f->active&&!f->journal&&!f->references&&name_ok(f->name);}
static void encode(StorageDelete *d,bool done){
 uint8_t *b=d->frame;const StorageRetentionFile *f=&d->victim;memset(b,0,256);memcpy(b,"RET1",4);put(b+4,1,2);put(b+6,done?2:1,2);put(b+8,d->owner_session,8);put(b+16,d->journal_id,8);put(b+24,d->operation,8);put(b+32,f->session_id,8);put(b+40,f->file_id,8);put(b+48,f->catalog_id,8);put(b+56,f->ledger_id,8);put(b+64,f->bytes,8);put(b+72,f->order_key,8);put(b+80,f->family,4);memcpy(b+88,f->name,strlen(f->name));put(b+252,crc32c(b,252),4);
}
bool storage_delete_decode(const uint8_t *b,size_t n,StorageDeleteRecord *out){
 if(!b||n!=256||!out||memcmp(b,"RET1",4)||get(b+4,2)!=1||(get(b+6,2)!=1&&get(b+6,2)!=2)||get(b+252,4)!=crc32c(b,252))return false;
 for(unsigned k=84;k<88;k++)if(b[k])return false;
 for(unsigned k=152;k<252;k++)if(b[k])return false;
 StorageDeleteRecord r={0};r.owner_session=get(b+8,8);r.journal_id=get(b+16,8);r.operation=get(b+24,8);r.complete=get(b+6,2)==2;r.file.session_id=get(b+32,8);r.file.file_id=get(b+40,8);r.file.catalog_id=get(b+48,8);r.file.ledger_id=get(b+56,8);r.file.bytes=get(b+64,8);r.file.order_key=get(b+72,8);r.file.family=(StorageRetentionFamily)get(b+80,4);r.file.closed=r.file.seal_verified=true;
 memcpy(r.file.name,b+88,64);if(!memchr(r.file.name,0,64)||!r.owner_session||!r.journal_id||!r.operation||!eligible(&r.file))return false;
 size_t len=strlen(r.file.name);for(size_t k=len;k<64;k++)if(b[88+k])return false;
 *out=r;return true;
}
bool storage_delete_init(StorageDelete *d,const StorageBackend *b,uint64_t session,uint64_t id,const StorageDeleteHooks *h){
 if(!d||!b||!b->size||!b->write||!b->sync||!session||!id||!h||!h->claim||!h->remove||!h->sync_media||!h->release)return false;
 StorageBackend copy=*b;StorageDeleteHooks hooks=*h;memset(d,0,sizeof(*d));d->journal=copy;d->hooks=hooks;d->owner_session=session;d->journal_id=id;d->next_operation=1;d->state=DELETE_SIZE;return true;
}
StorageResult storage_delete_request(StorageDelete *d,const StorageRetentionFile *f){
 if(!d||!eligible(f)||f->file_id==d->journal_id)return STORE_INVALID;
 if(d->state!=DELETE_IDLE)return STORE_BUSY;
 if(d->next_operation==UINT64_MAX)return STORE_EXHAUSTED;
 d->victim=*f;d->operation=d->next_operation++;d->state=DELETE_CLAIM;return STORE_OK;
}
void storage_delete_step(StorageDelete *d){
 if(!d)return;
 size_t actual=0;StorageIoResult io;
 switch(d->state){
 case DELETE_SIZE:{uint64_t n=0;if(d->journal.size(d->journal.context,&n)!=STORE_IO_OK||n)d->state=DELETE_FAULT;else d->state=DELETE_IDLE;break;}
 case DELETE_CLAIM:{StorageResult r=d->hooks.claim(d->hooks.context,&d->victim);if(r==STORE_BUSY)break;if(r!=STORE_OK){d->state=DELETE_FAULT;break;}d->held=true;encode(d,false);d->state=DELETE_INTENT_WRITE;break;}
 case DELETE_INTENT_WRITE:case DELETE_DONE_WRITE:
  if(d->bytes>UINT64_MAX-256){d->state=DELETE_FAULT;break;}
  io=d->journal.write(d->journal.context,d->bytes,d->frame,256,&actual);
  if(io!=STORE_IO_OK||actual!=256){d->state=DELETE_FAULT;break;}
  d->bytes+=256;d->state=d->state==DELETE_INTENT_WRITE?DELETE_INTENT_SYNC:DELETE_DONE_SYNC;break;
 case DELETE_INTENT_SYNC:if(d->journal.sync(d->journal.context)!=STORE_IO_OK)d->state=DELETE_FAULT;else d->state=DELETE_UNLINK;break;
 case DELETE_UNLINK:if(d->hooks.remove(d->hooks.context,&d->victim,&d->missing)!=STORE_IO_OK)d->state=DELETE_FAULT;else d->state=DELETE_MEDIA_SYNC;break;
 case DELETE_MEDIA_SYNC:if(d->hooks.sync_media(d->hooks.context)!=STORE_IO_OK)d->state=DELETE_FAULT;else{encode(d,true);d->state=DELETE_DONE_WRITE;}break;
 case DELETE_DONE_SYNC:if(d->journal.sync(d->journal.context)!=STORE_IO_OK)d->state=DELETE_FAULT;else d->state=DELETE_RELEASE;break;
 case DELETE_RELEASE:d->hooks.release(d->hooks.context,&d->victim);d->held=false;d->state=DELETE_IDLE;break;
 default:break;
 }
}
static bool same(const StorageDeleteRecord *a,const StorageDeleteRecord *b){return a->operation==b->operation&&a->owner_session==b->owner_session&&a->journal_id==b->journal_id&&a->file.session_id==b->file.session_id&&a->file.file_id==b->file.file_id&&a->file.catalog_id==b->file.catalog_id&&a->file.ledger_id==b->file.ledger_id&&a->file.bytes==b->file.bytes&&a->file.order_key==b->file.order_key&&a->file.family==b->file.family&&!strcmp(a->file.name,b->file.name);}
bool storage_delete_scan_init(StorageDeleteScan *s,const StorageBackend *b,uint64_t session,uint64_t id,uint64_t target_session,uint64_t file){
 if(!s||!b||!b->size||!b->read||!session||!id||!target_session||!file)return false;
 StorageBackend copy=*b;memset(s,0,sizeof(*s));s->backend=copy;s->session=session;s->journal_id=id;s->target_session=target_session;s->target_file=file;s->state=DELETE_SCAN_SIZE;return true;
}
void storage_delete_scan_step(StorageDeleteScan *s){
 if(!s)return;
 if(s->state==DELETE_SCAN_SIZE){if(s->backend.size(s->backend.context,&s->size)!=STORE_IO_OK)s->state=DELETE_SCAN_IO;else s->state=DELETE_SCAN_READ;return;}
 if(s->state!=DELETE_SCAN_READ)return;
 if(s->prefix==s->size){s->state=s->have_pending?DELETE_SCAN_TAIL:DELETE_SCAN_DONE;return;}
 if(s->size-s->prefix<256){s->state=DELETE_SCAN_TAIL;return;}
 size_t actual=0;StorageDeleteRecord r;
 if(s->backend.read(s->backend.context,s->prefix,s->frame,256,&actual)!=STORE_IO_OK||actual!=256){s->state=DELETE_SCAN_IO;return;}
 if(!storage_delete_decode(s->frame,256,&r)||r.owner_session!=s->session||r.journal_id!=s->journal_id||(s->have_pending?(!r.complete||!same(&r,&s->pending)):(r.complete||r.operation<=s->last_operation))){s->state=DELETE_SCAN_TAIL;return;}
 if(!s->have_pending){s->pending=r;s->last_operation=r.operation;s->have_pending=true;}else s->have_pending=false;
 if(r.file.session_id==s->target_session&&r.file.file_id==s->target_file){s->found=r;s->have_found=true;}
 s->prefix+=256;
}
