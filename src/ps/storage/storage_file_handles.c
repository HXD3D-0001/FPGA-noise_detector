#include "storage_file_handles.h"
#include <string.h>
static bool owner(StorageFileHandles *p){return p&&p->ops.is_owner&&p->ops.is_owner(p->ops.context);}
static StorageHandle *entry(StorageFileHandles *p,unsigned k){return k<STORAGE_HANDLES&&p->files[k].used?&p->files[k]:NULL;}
static bool canonical(const char *s,size_t capacity,char out[192]){
 if(!s||!s[0]||!memchr(s,0,capacity))return false;
 unsigned used=0,base=0,ext=0;bool dot=false;
 for(unsigned k=0;;k++){
  unsigned char c=(unsigned char)s[k];
  if(!c||c=='/'){
   if(!base||base>8||ext>3||(dot&&!ext))return false;
   if(!c){out[used]=0;return true;}
   if(used>=190)return false;
   out[used++]='/';base=ext=0;dot=false;continue;
  }
  if(used>=191)return false;
  if(c=='.'){if(dot||!base)return false;dot=true;}
  else {if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'))return false;if(dot)ext++;else base++;}
  out[used++]=(char)(c>='a'&&c<='z'?c-'a'+'A':c);
 }
}
bool storage_file_handles_init(StorageFileHandles *p,const StorageFileOps *o){
 if(!p||!o||!o->is_owner||!o->open||!o->close||!o->remove||!o->sync_media||!o->is_owner(o->context))return false;
 StorageFileOps copy=*o;memset(p,0,sizeof(*p));p->ops=copy;p->next_pin=1;p->deletion_slot=STORAGE_HANDLES;return true;
}
StorageResult storage_file_handles_register(StorageFileHandles *p,const StorageHandleSpec *s,unsigned *slot){
 if(!owner(p)||!s||!slot||!s->session_id||!s->file_id||(unsigned)s->kind>HANDLE_AUXILIARY||(unsigned)s->family>=RETAIN_FAMILIES)return STORE_INVALID;
 if(s->kind==HANDLE_DATA&&(!s->catalog_id||!s->ledger_id||s->file_id==s->catalog_id||s->file_id==s->ledger_id||s->catalog_id==s->ledger_id))return STORE_INVALID;
 StorageHandleSpec copy=*s;if(!canonical(s->path,sizeof(s->path),copy.path))return STORE_INVALID;
 unsigned free=STORAGE_HANDLES;
 for(unsigned k=0;k<STORAGE_HANDLES;k++){StorageHandle *e=&p->files[k];if(!e->used){if(free==STORAGE_HANDLES)free=k;continue;}if((e->spec.session_id==s->session_id&&e->spec.file_id==s->file_id)||!strcmp(e->spec.path,copy.path))return STORE_INVALID;}
 if(free==STORAGE_HANDLES)return STORE_CAPACITY;
 StorageHandle *e=&p->files[free];memset(e,0,sizeof(*e));e->used=true;e->spec=copy;*slot=free;return STORE_OK;
}
StorageResult storage_file_handles_attach(StorageFileHandles *p,unsigned k,const StorageBackend *b){
 StorageHandle *e;if(!owner(p)||!(e=entry(p,k))||!b||!b->size||!b->read||!b->write||!b->sync)return STORE_INVALID;
 if(e->opened||e->active||e->sealed||e->references||e->closing||e->claimed||e->removed)return STORE_BUSY;
 /* Exclude explicit backend aliases; hidden filesystem aliases remain a port
  * obligation (concrete FatFs adapter rejects canonical duplicate paths). */
 for(unsigned j=0;j<STORAGE_HANDLES;j++)if(p->files[j].used&&p->files[j].opened&&p->files[j].backend.context==b->context&&p->files[j].backend.read==b->read)return STORE_INVALID;
 e->backend=*b;e->opened=e->active=true;return STORE_OK;
}
static bool same_seal(const StorageCatalogEntry *a,const StorageCatalogEntry *b){
 return a->state==CATALOG_SEALED&&b->state==CATALOG_SEALED&&a->descriptor.file_id==b->descriptor.file_id&&a->descriptor.role==b->descriptor.role&&a->descriptor.created_us==b->descriptor.created_us&&a->descriptor.created_known==b->descriptor.created_known&&a->descriptor.location==b->descriptor.location&&a->descriptor.ordinal==b->descriptor.ordinal&&!memcmp(a->descriptor.name,b->descriptor.name,64)&&a->revision==b->revision&&a->file_bytes==b->file_bytes&&a->family_prefix==b->family_prefix&&a->records==b->records&&a->first_seq==b->first_seq&&a->last_seq==b->last_seq&&a->sealed_us==b->sealed_us&&a->sealed_known==b->sealed_known&&a->admission_cutoff==b->admission_cutoff&&a->file_crc==b->file_crc&&a->definitions==b->definitions;
}
StorageResult storage_file_handles_seal(StorageFileHandles *p,unsigned k,const StorageCatalogEntry *s){
 StorageHandle *e;if(!owner(p)||!(e=entry(p,k))||!s||e->spec.kind!=HANDLE_DATA||s->state!=CATALOG_SEALED||s->descriptor.file_id!=e->spec.file_id||!memchr(s->descriptor.name,0,64))return STORE_INVALID;
 const char *name=strrchr(e->spec.path,'/');name=name?name+1:e->spec.path;
 char normalized[192];if(!canonical(s->descriptor.name,sizeof(s->descriptor.name),normalized)||strcmp(name,normalized))return STORE_INVALID;
 if(e->claimed||e->closing||e->removed||e->references)return STORE_BUSY;
 if(e->sealed)return same_seal(&e->seal,s)?STORE_OK:STORE_INVALID;
 e->seal=*s;e->sealed=true;e->active=false;e->verified=false;return STORE_OK;
}
StorageResult storage_file_handles_open_read(StorageFileHandles *p,unsigned k){
 StorageHandle *e;if(!owner(p)||!(e=entry(p,k)))return STORE_INVALID;
 if(e->active||e->closing||e->claimed||e->removed)return STORE_BUSY;
 if(e->opened)return STORE_OK;
 StorageBackend b={0};if(p->ops.open(p->ops.context,k,e->spec.path,&b)!=STORE_IO_OK)return STORE_INVALID;
 /* Bad adapter contract keeps opened handle protected for explicit close. */
 e->opened=true;e->backend=b;
 return b.size&&b.read?STORE_OK:STORE_INVALID;
}
StorageResult storage_file_handles_freeze_aux(StorageFileHandles *p,unsigned k,const StorageFileCatalog *c){
 StorageHandle *e;if(!owner(p)||!(e=entry(p,k))||!c||!c->owner||!c->owner->family||e->spec.kind!=HANDLE_AUXILIARY||c->phase!=CATALOG_IDLE||!c->count||c->count>STORAGE_FAMILY_FILES||c->owner->family->phase!=FAMILY_READY||c->count!=c->owner->family->file_count||c->owner->family->session_id!=e->spec.session_id||(e->spec.file_id!=c->catalog_id&&e->spec.file_id!=c->owner->family->ledger_id)||e->claimed||e->removed)return STORE_INVALID;
 for(unsigned j=0;j<c->count;j++)if(c->files[j].state!=CATALOG_SEALED)return STORE_BUSY;
 e->active=false;return STORE_OK;
}
static StorageResult pin(StorageFileHandles *p,unsigned k,bool query,uint64_t *out){
 StorageHandle *e=entry(p,k);if(!e||!out)return STORE_INVALID;
 if(!e->opened||e->closing||e->claimed||e->removed)return STORE_BUSY;
 if(p->next_pin==UINT64_MAX)return STORE_EXHAUSTED;
 for(unsigned j=0;j<STORAGE_FILE_PINS;j++)if(!p->pins[j].used){p->pins[j]=(StorageFilePin){.token=p->next_pin++,.file=k,.used=true,.query=query};e->references++;*out=p->pins[j].token;return STORE_OK;}
 return STORE_CAPACITY;
}
StorageResult storage_file_handles_pin(StorageFileHandles *p,unsigned k,uint64_t *out){return owner(p)?pin(p,k,false,out):STORE_INVALID;}
static StorageIoResult unpin(StorageFileHandles *p,uint64_t token,bool query){
 if(!owner(p)||!token)return STORE_IO_ERROR;
 for(unsigned j=0;j<STORAGE_FILE_PINS;j++)if(p->pins[j].used&&p->pins[j].token==token){
  StorageFilePin *r=&p->pins[j];StorageHandle *e=entry(p,r->file);
  if(!e||!e->references||r->query!=query)return STORE_IO_ERROR;
  if(e->references==1&&!e->active&&e->opened){if(p->ops.close(p->ops.context,r->file)!=STORE_IO_OK){r->failed_release=true;e->closing=true;return STORE_IO_ERROR;}e->opened=false;memset(&e->backend,0,sizeof(e->backend));}
  e->references--;r->used=false;return STORE_IO_OK;
 }
 return STORE_IO_ERROR;
}
StorageIoResult storage_file_handles_unpin(StorageFileHandles *p,uint64_t token){return unpin(p,token,false);}
StorageIoResult storage_file_handles_retry_query_release(StorageFileHandles *p,uint64_t token){
 if(!owner(p)||!token)return STORE_IO_ERROR;
 for(unsigned j=0;j<STORAGE_FILE_PINS;j++)if(p->pins[j].used&&p->pins[j].token==token&&p->pins[j].query&&p->pins[j].failed_release)return unpin(p,token,true);
 return STORE_IO_ERROR;
}
StorageResult storage_file_handles_close(StorageFileHandles *p,unsigned k){
 StorageHandle *e;if(!owner(p)||!(e=entry(p,k)))return STORE_INVALID;
 if(e->active||e->claimed)return STORE_BUSY;
 e->closing=true;if(e->references)return STORE_BUSY;
 if(e->opened){if(p->ops.close(p->ops.context,k)!=STORE_IO_OK)return STORE_INVALID;e->opened=false;memset(&e->backend,0,sizeof(e->backend));}
 return STORE_OK;
}
StorageResult storage_file_handles_freeze_epoch(StorageFileHandles *p,const StorageFileCatalog *c,const unsigned *slots){
 if(!owner(p)||!c||!c->owner||!c->owner->family||!slots||!c->count||c->count>STORAGE_FAMILY_FILES||c->phase!=CATALOG_IDLE||c->owner->family->phase!=FAMILY_READY)return STORE_INVALID;
 uint64_t session=c->owner->family->session_id,ledger=c->owner->family->ledger_id;
 for(unsigned k=0;k<c->count+2;k++){
  if(slots[k]>=STORAGE_HANDLES)return STORE_INVALID;
  for(unsigned j=0;j<k;j++)if(slots[j]==slots[k])return STORE_INVALID;
  StorageHandle *e=entry(p,slots[k]);uint64_t id=k<c->count?c->files[k].descriptor.file_id:k==c->count?ledger:c->catalog_id;
  if(!e||e->spec.session_id!=session||e->spec.file_id!=id||e->claimed||e->removed||e->expired)return STORE_INVALID;
  if(k<c->count){
   const StorageCatalogEntry *seal=&c->files[k];char normalized[192];const char *base=strrchr(e->spec.path,'/');base=base?base+1:e->spec.path;
   if(e->spec.kind!=HANDLE_DATA||e->spec.catalog_id!=c->catalog_id||e->spec.ledger_id!=ledger||seal->state!=CATALOG_SEALED||!canonical(seal->descriptor.name,64,normalized)||strcmp(base,normalized))return STORE_INVALID;
   if(e->sealed){if(!same_seal(&e->seal,seal))return STORE_INVALID;}else if(!e->opened||!e->active)return STORE_INVALID;
  }else if(e->spec.kind!=HANDLE_AUXILIARY||(!e->active&&!e->closing)||(!e->opened&&!e->closing))return STORE_INVALID;
 }
 /* No IO/interleaving in this executor. Validate whole group before mutation. */
 for(unsigned k=0;k<c->count+2;k++){
  StorageHandle *e=&p->files[slots[k]];if(k<c->count){e->seal=c->files[k];e->sealed=true;e->verified=false;}e->active=false;e->closing=true;
 }
 return STORE_OK;
}
StorageResult storage_file_handles_finish_epoch(StorageFileHandles *p,const StorageFileCatalog *c,const unsigned *slots){
 StorageResult r=storage_file_handles_freeze_epoch(p,c,slots);if(r!=STORE_OK)return r;
 for(unsigned k=0;k<c->count+2;k++){StorageHandle *e=&p->files[slots[k]];if(e->opened||e->references)return STORE_BUSY;}
 for(unsigned k=0;k<c->count+2;k++)p->files[slots[k]].closing=false;
 return STORE_OK;
}
StorageResult storage_file_handles_finish_close(StorageFileHandles *p,unsigned k){
 StorageHandle *e;if(!owner(p)||!(e=entry(p,k)))return STORE_INVALID;
 if(!e->closing||e->opened||e->references||e->claimed)return STORE_BUSY;
 e->closing=false;return STORE_OK;
}
StorageResult storage_file_handles_forget(StorageFileHandles *p,unsigned k){
 StorageHandle *e;if(!owner(p)||!(e=entry(p,k)))return STORE_INVALID;
 if(e->active||e->opened||e->references||e->claimed||e->closing)return STORE_BUSY;
 memset(e,0,sizeof(*e));return STORE_OK;
}
static bool same_backend(const StorageBackend *a,const StorageBackend *b){return a->context==b->context&&a->read==b->read&&a->size==b->size;}
StorageResult storage_file_handles_revalidated(StorageFileHandles *p,unsigned k,const StorageArchiveVerifier *v){
 StorageHandle *e;if(!owner(p)||!(e=entry(p,k))||!v||!v->reader||v->state!=ARCHIVE_READ_REVALIDATED||!e->sealed||!e->opened||!e->references||e->closing||e->claimed||e->removed||e->spec.session_id!=v->session_id||e->spec.ledger_id!=v->ledger_id||!same_seal(&e->seal,&v->recorded)||!same_backend(&e->backend,&v->reader->backend))return STORE_INVALID;
 for(unsigned j=0;j<STORAGE_HANDLES;j++){StorageHandle *l=&p->files[j];if(l->used&&l->spec.kind==HANDLE_AUXILIARY&&l->spec.session_id==e->spec.session_id&&l->spec.file_id==e->spec.ledger_id&&l->opened&&l->references&&!l->active&&!l->claimed&&!l->closing&&!l->removed&&same_backend(&l->backend,&v->ledger)){e->verified=true;return STORE_OK;}}
 return STORE_DEPENDENCY;
}
StorageResult storage_file_handles_inventory(StorageFileHandles *p,unsigned k,StorageRetentionFile *out){
 StorageHandle *e;if(!owner(p)||!out||!(e=entry(p,k)))return STORE_INVALID;
 if(e->expired)return STORE_EXPIRED;
 StorageRetentionFile f={0};f.session_id=e->spec.session_id;f.file_id=e->spec.file_id;f.catalog_id=e->spec.catalog_id;f.ledger_id=e->spec.ledger_id;f.order_key=e->spec.order_key;f.family=e->spec.family;f.bytes=e->sealed?e->seal.file_bytes:0;f.references=e->references;f.active=e->active||e->closing||e->claimed||!e->sealed;f.journal=e->spec.kind!=HANDLE_DATA;f.closed=!e->opened&&!e->active;f.seal_verified=e->verified;
 const char *name=strrchr(e->spec.path,'/');name=name?name+1:e->spec.path;memcpy(f.name,name,strlen(name)+1);*out=f;return STORE_OK;
}
static StorageQueryOpen query_open(void *ctx,const StorageQueryRequest *r,StorageQuerySource *s){
 StorageFileHandles *p=ctx;if(!owner(p)||!r||!s)return QUERY_SOURCE_IO;
 for(unsigned k=0;k<STORAGE_HANDLES;k++){StorageHandle *e=&p->files[k];if(!e->used||e->spec.session_id!=r->session_id||e->spec.file_id!=r->file_id)continue;
  if(e->expired)return QUERY_SOURCE_EXPIRED;
  if(e->spec.kind!=HANDLE_DATA||!e->sealed||e->active||e->closing||e->claimed||e->removed)return QUERY_SOURCE_NOT_READY;
  bool available=false;for(unsigned j=0;j<STORAGE_FILE_PINS;j++)if(!p->pins[j].used)available=true;
  if(!available||p->next_pin==UINT64_MAX)return QUERY_SOURCE_NOT_READY;
  if(storage_file_handles_open_read(p,k)!=STORE_OK)return QUERY_SOURCE_IO;
  uint64_t token=0;if(pin(p,k,true,&token)!=STORE_OK)return QUERY_SOURCE_IO;
  *s=(StorageQuerySource){.backend=e->backend,.session_id=e->spec.session_id,.file_id=e->spec.file_id,.bytes=e->seal.file_bytes,.pin=token,.exact_size=true,.check_crc=true,.expected_crc=e->seal.file_crc,.provenance=QUERY_HISTORICAL_CLAIM_UNVERIFIED};return QUERY_SOURCE_OK;
 }
 return QUERY_SOURCE_NOT_READY;
}
static StorageIoResult query_release(void *ctx,StorageQuerySource *s){
 StorageFileHandles *p=ctx;if(!owner(p)||!s||!s->pin)return STORE_IO_ERROR;
 for(unsigned j=0;j<STORAGE_FILE_PINS;j++){StorageFilePin *pin=&p->pins[j];if(!pin->used||!pin->query||pin->token!=s->pin)continue;
  StorageHandle *e=entry(p,pin->file);
  if(!e||e->spec.session_id!=s->session_id||e->spec.file_id!=s->file_id||!same_backend(&e->backend,&s->backend)||s->bytes!=e->seal.file_bytes||s->expected_crc!=e->seal.file_crc||!s->exact_size||!s->check_crc||s->provenance!=QUERY_HISTORICAL_CLAIM_UNVERIFIED)return STORE_IO_ERROR;
  return unpin(p,s->pin,true);
 }
 return STORE_IO_ERROR;
}
StorageQueryFiles storage_file_handles_query(StorageFileHandles *p){return (StorageQueryFiles){p,query_open,query_release};}
static bool same_victim(const StorageRetentionFile *a,const StorageRetentionFile *b){return a->session_id==b->session_id&&a->file_id==b->file_id&&a->catalog_id==b->catalog_id&&a->ledger_id==b->ledger_id&&a->bytes==b->bytes&&a->order_key==b->order_key&&a->family==b->family&&!strcmp(a->name,b->name);}
static StorageResult claim(void *ctx,const StorageRetentionFile *v){
 StorageFileHandles *p=ctx;if(!owner(p)||!v||!memchr(v->name,0,sizeof(v->name)))return STORE_INVALID;
 if(p->deletion_slot!=STORAGE_HANDLES)return STORE_BUSY;
 for(unsigned k=0;k<STORAGE_HANDLES;k++){StorageHandle *e=&p->files[k];if(!e->used||e->spec.session_id!=v->session_id||e->spec.file_id!=v->file_id)continue;
  StorageRetentionFile now;if(storage_file_handles_inventory(p,k,&now)!=STORE_OK||!same_victim(&now,v))return STORE_INVALID;
  if(now.active||now.journal||!now.closed||!now.seal_verified||now.references||e->removed)return STORE_BUSY;
  e->claimed=true;p->deletion_slot=k;return STORE_OK;
 }
 return STORE_EXPIRED;
}
static StorageIoResult remove_file(void *ctx,const StorageRetentionFile *v,bool *missing){
 StorageFileHandles *p=ctx;if(!owner(p)||!v||!missing)return STORE_IO_ERROR;
 StorageHandle *e=entry(p,p->deletion_slot);StorageRetentionFile now;
 if(!e||!e->claimed||e->references||e->opened||e->removed||storage_file_handles_inventory(p,p->deletion_slot,&now)!=STORE_OK||!same_victim(&now,v))return STORE_IO_ERROR;
 StorageIoResult r=p->ops.remove(p->ops.context,e->spec.path,missing);if(r==STORE_IO_OK)e->removed=true;return r;
}
static StorageIoResult sync_media(void *ctx){StorageFileHandles *p=ctx;if(!owner(p))return STORE_IO_ERROR;StorageHandle *e=entry(p,p->deletion_slot);return e&&e->claimed&&e->removed?p->ops.sync_media(p->ops.context):STORE_IO_ERROR;}
static void release_delete(void *ctx,const StorageRetentionFile *v){
 StorageFileHandles *p=ctx;if(!owner(p)||!v)return;
 StorageHandle *e=entry(p,p->deletion_slot);StorageRetentionFile now;
 if(!e||!e->claimed||!e->removed||storage_file_handles_inventory(p,p->deletion_slot,&now)!=STORE_OK||!same_victim(&now,v))return;
 e->expired=true;e->claimed=false;e->verified=false;p->deletion_slot=STORAGE_HANDLES;
}
StorageDeleteHooks storage_file_handles_delete(StorageFileHandles *p){return (StorageDeleteHooks){p,claim,remove_file,sync_media,release_delete};}
