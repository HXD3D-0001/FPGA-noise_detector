#include "storage_inventory.h"
#include <string.h>
enum { I_ACQUIRE,I_OPEN_DIR,I_NEXT,I_OPEN_META,I_META_SIZE,I_FIRST,I_SCAN,I_CLOSE_META,I_CLOSE_DIR,I_STAT_DATA,I_STAT_LEDGER,I_FINISH,I_CLEAN_FILE,I_CLEAN_DIR,I_HELD,I_DONE };
static bool owned(const StorageInventory *s){return s&&s->ops.is_owner&&s->ops.is_owner(s->ops.context);}
static uint64_t get(const uint8_t *b){uint64_t n=0;for(unsigned k=0;k<8;k++)n|=(uint64_t)b[k]<<(8*k);return n;}
static bool name(const char *s,size_t cap,char *out){
 if(!s||!memchr(s,0,cap))return false;
 unsigned base=0,ext=0,used=0;bool dot=false;
 for(unsigned k=0;s[k];k++){unsigned char c=(unsigned char)s[k];if(c=='.'){if(dot||!base)return false;dot=true;}else{if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'))return false;if(dot)ext++;else base++;}if(used>=12)return false;out[used++]=(char)(c>='a'&&c<='z'?c-'a'+'A':c);}
 if(!base||base>8||ext>3||(dot&&!ext))return false;
 out[used]=0;return true;
}
static bool root_path(const char *s,char *out){
 if(!s||!memchr(s,0,192)||!s[0])return false;
 size_t used=0,start=0;char part[64],canonical[64];
 for(size_t k=0;;k++)if(s[k]=='/'||!s[k]){size_t n=k-start;if(!n||n>=64)return false;memcpy(part,s+start,n);part[n]=0;if(!name(part,64,canonical))return false;size_t c=strlen(canonical);if(used+c+1>=192)return false;memcpy(out+used,canonical,c);used+=c;if(!s[k]){out[used]=0;return true;}out[used++]='/';start=k+1;}
}
static bool join(const char *dir,const char *base,char *out){size_t a=strlen(dir),b=strlen(base);if(a+b+2>192)return false;memcpy(out,dir,a);out[a]='/';memcpy(out+a+1,base,b+1);return true;}
bool storage_inventory_init(StorageInventory *s,const StorageInventoryOps *o,const StorageInventoryLayout *l,uint64_t session,uint64_t file){
 if(!s||!o||!l||!session||!file||!o->is_owner||!o->acquire||!o->version||!o->release||!o->open_directory||!o->next||!o->close_directory||!o->open_file||!o->close_file||!o->stat||!o->is_owner(o->context))return false;
 StorageInventoryOps ops=*o;StorageInventoryLayout layout={0};
 if(!root_path(l->root,layout.root)||!name(l->catalog_name,64,layout.catalog_name)||!name(l->ledger_name,64,layout.ledger_name)||!name(l->delete_name,64,layout.delete_name)||!strcmp(layout.catalog_name,layout.ledger_name)||!strcmp(layout.catalog_name,layout.delete_name)||!strcmp(layout.ledger_name,layout.delete_name))return false;
 for(unsigned k=0;k<RETAIN_FAMILIES;k++)if(l->family_names[k][0]){if(!name(l->family_names[k],64,layout.family_names[k])||!strcmp(layout.family_names[k],layout.catalog_name)||!strcmp(layout.family_names[k],layout.ledger_name)||!strcmp(layout.family_names[k],layout.delete_name))return false;for(unsigned j=0;j<k;j++)if(!strcmp(layout.family_names[k],layout.family_names[j]))return false;}
 memset(s,0,sizeof(*s));s->ops=ops;s->layout=layout;s->result.session_id=session;s->result.file_id=file;s->result.status=INVENTORY_RUNNING;s->desired=INVENTORY_ABSENT;strcpy(s->paths[0],layout.root);s->phase=I_ACQUIRE;return true;
}
static void stop(StorageInventory *s,StorageInventoryStatus status){s->desired=status;s->phase=s->file_open?I_CLEAN_FILE:I_CLEAN_DIR;}
void storage_inventory_cancel(StorageInventory *s){if(owned(s)&&s->phase!=I_DONE)s->cancel=true;}
bool storage_inventory_ready(const StorageInventory *s){return s&&s->phase==I_DONE&&!s->snapshot&&!s->file_open;}
bool storage_inventory_retry_cleanup(StorageInventory *s){if(!owned(s)||s->phase!=I_HELD)return false;s->phase=s->file_open?I_CLEAN_FILE:I_CLEAN_DIR;return true;}
static void catalog_result(StorageInventory *s){
 if(s->catalog.state!=CATALOG_REPLAY_DONE){s->incomplete=true;return;}
 for(unsigned k=0;k<s->catalog.count;k++){StorageCatalogEntry *e=&s->catalog.files[k];
  char base[64];if(!name(e->descriptor.name,64,base)){s->incomplete=true;return;}
  unsigned family=RETAIN_FAMILIES;for(unsigned j=0;j<RETAIN_FAMILIES;j++)if(!strcmp(base,s->layout.family_names[j]))family=j;
  if(family==RETAIN_FAMILIES||(e->descriptor.location==CATALOG_BASE&&(s->declared[s->depth]&(1u<<family)))){s->incomplete=true;return;}
  if(e->descriptor.location==CATALOG_BASE)s->declared[s->depth]|=1u<<family;
  if(s->catalog.session_id!=s->result.session_id||e->descriptor.file_id!=s->result.file_id)continue;
  if(s->result.have_catalog){s->incomplete=true;return;}
  StorageHandleSpec spec={.session_id=s->result.session_id,.file_id=s->result.file_id,.catalog_id=s->catalog.catalog_id,.ledger_id=s->catalog.ledger_id,.kind=HANDLE_DATA,.family=(StorageRetentionFamily)family,.order_key=e->descriptor.created_known?e->descriptor.created_us:0};
  if(!storage_file_catalog_resolve(s->file_path,&e->descriptor,spec.path,sizeof(spec.path))||!join(s->paths[s->depth],s->layout.ledger_name,s->result.ledger_path)){s->incomplete=true;return;}
  s->result.have_catalog=true;s->result.spec=spec;s->result.recorded=*e;
 }
}
static void delete_result(StorageInventory *s){
 if(s->deletion.state!=DELETE_SCAN_DONE){s->incomplete=true;return;}
 if(!s->deletion.have_found)return;
 if(s->result.have_delete){s->incomplete=true;return;}
 s->result.have_delete=true;s->result.deletion=s->deletion.found;
}
static bool matching_delete(StorageInventory *s){
 StorageDeleteRecord *d=&s->result.deletion;StorageCatalogEntry *e=&s->result.recorded;char canonical[64];
 if(!s->result.have_delete||!d->complete||!name(d->file.name,64,canonical))return false;
 const char *base=strrchr(s->result.spec.path,'/');base=base?base+1:s->result.spec.path;
 return d->file.session_id==s->result.session_id&&d->file.file_id==s->result.file_id&&d->file.catalog_id==s->result.spec.catalog_id&&d->file.ledger_id==s->result.spec.ledger_id&&d->file.bytes==e->file_bytes&&d->file.family==s->result.spec.family&&!strcmp(base,canonical);
}
void storage_inventory_step(StorageInventory *s){
 if(!owned(s)||s->phase==I_DONE||s->phase==I_HELD)return;
 if(s->cancel&&s->phase<I_FINISH){stop(s,INVENTORY_CANCELLED);s->cancel=false;return;}
 if(s->snapshot&&s->phase<I_FINISH&&s->ops.version(s->ops.context)!=s->result.version){stop(s,INVENTORY_INCOMPLETE);return;}
 switch(s->phase){
 case I_ACQUIRE:{uint64_t v=0;StorageResult r=s->ops.acquire(s->ops.context,s->layout.root,&v);if(r==STORE_BUSY)return;if(r!=STORE_OK){stop(s,INVENTORY_IO);return;}s->snapshot=true;s->result.version=v;if(!v){stop(s,INVENTORY_IO);return;}s->phase=I_OPEN_DIR;return;}
 case I_OPEN_DIR:
  if(s->ops.open_directory(s->ops.context,s->depth,s->paths[s->depth])!=STORE_IO_OK){stop(s,INVENTORY_IO);return;}s->dirs[s->depth]=true;s->data_seen[s->depth]=s->declared[s->depth]=0;s->result.directories++;s->phase=I_NEXT;return;
 case I_NEXT:{
  StorageDirectoryItem item={0};bool have=false;
  if(s->ops.next(s->ops.context,s->depth,&item,&have)!=STORE_IO_OK){stop(s,INVENTORY_IO);return;}
  if(!have){s->phase=I_CLOSE_DIR;return;}
  if(!memchr(item.name,0,sizeof(item.name))){stop(s,INVENTORY_INCOMPLETE);return;}
  if(!strcmp(item.name,".")||!strcmp(item.name,".."))return;
  char base[64];if(!name(item.name,sizeof(item.name),base)){stop(s,INVENTORY_INCOMPLETE);return;}
  if(item.directory){/* EVENT_TREE is resolved through its parent catalog. */if(!strcmp(base,"EV"))return;if(!strcmp(base,s->layout.catalog_name)||!strcmp(base,s->layout.delete_name)){stop(s,INVENTORY_INCOMPLETE);return;}if(s->depth+1==STORAGE_INVENTORY_DEPTH){stop(s,INVENTORY_CAPACITY);return;}if(!join(s->paths[s->depth],base,s->paths[s->depth+1])){stop(s,INVENTORY_CAPACITY);return;}s->depth++;s->phase=I_OPEN_DIR;return;}
  s->result.files++;for(unsigned j=0;j<RETAIN_FAMILIES;j++)if(!strcmp(base,s->layout.family_names[j]))s->data_seen[s->depth]|=1u<<j;
  s->metadata_kind=!strcmp(base,s->layout.catalog_name)?1:!strcmp(base,s->layout.delete_name)?2:0;
  if(!s->metadata_kind)return;
  if(!join(s->paths[s->depth],base,s->file_path)){stop(s,INVENTORY_CAPACITY);return;}
  if(s->metadata_kind==1)s->result.catalogs++;else s->result.delete_logs++;
  s->phase=I_OPEN_META;return;
 }
 case I_OPEN_META:
  memset(&s->backend,0,sizeof(s->backend));
  if(s->ops.open_file(s->ops.context,s->file_path,&s->backend)!=STORE_IO_OK){stop(s,INVENTORY_IO);return;}
  s->file_open=true;if(!s->backend.size||!s->backend.read){stop(s,INVENTORY_IO);return;}s->phase=I_META_SIZE;return;
 case I_META_SIZE:{uint64_t n=0;if(s->backend.size(s->backend.context,&n)!=STORE_IO_OK){stop(s,INVENTORY_IO);return;}if(n<256){/* Empty delete log is valid; empty catalog has no identity. */if(n||s->metadata_kind==1)s->incomplete=true;s->phase=I_CLOSE_META;return;}s->phase=I_FIRST;return;}
 case I_FIRST:{
  size_t actual=0;if(s->backend.read(s->backend.context,0,s->first,256,&actual)!=STORE_IO_OK||actual!=256){stop(s,INVENTORY_IO);return;}
  bool valid=false;
  if(s->metadata_kind==1){StorageCatalogEntry e;uint64_t session=get(s->first+8),cat=get(s->first+16),ledger=get(s->first+24);valid=storage_file_catalog_decode(s->first,256,session,cat,ledger,&e)&&storage_catalog_replay_init(&s->catalog,&s->backend,session,cat,ledger);}
  else {StorageDeleteRecord r;valid=storage_delete_decode(s->first,256,&r)&&storage_delete_scan_init(&s->deletion,&s->backend,r.owner_session,r.journal_id,s->result.session_id,s->result.file_id);}
  if(!valid){s->incomplete=true;s->phase=I_CLOSE_META;return;}s->phase=I_SCAN;return;
 }
 case I_SCAN:
  if(s->metadata_kind==1){storage_catalog_replay_step(&s->catalog);if(s->catalog.state==CATALOG_REPLAY_SIZE||s->catalog.state==CATALOG_REPLAY_FRAME)return;if(s->catalog.state==CATALOG_REPLAY_IO){stop(s,INVENTORY_IO);return;}catalog_result(s);}
  else {storage_delete_scan_step(&s->deletion);if(s->deletion.state==DELETE_SCAN_SIZE||s->deletion.state==DELETE_SCAN_READ)return;if(s->deletion.state==DELETE_SCAN_IO){stop(s,INVENTORY_IO);return;}delete_result(s);}
  s->phase=I_CLOSE_META;return;
 case I_CLOSE_META:
  if(s->ops.close_file(s->ops.context)!=STORE_IO_OK){s->desired=INVENTORY_IO;s->phase=I_HELD;return;}
  s->file_open=false;s->phase=I_NEXT;return;
 case I_CLOSE_DIR:
  if(s->data_seen[s->depth]&~s->declared[s->depth])s->incomplete=true;
  if(s->ops.close_directory(s->ops.context,s->depth)!=STORE_IO_OK){s->desired=INVENTORY_IO;s->phase=I_HELD;return;}
  s->dirs[s->depth]=false;if(s->depth){s->depth--;s->phase=I_NEXT;return;}
  if(s->incomplete){stop(s,INVENTORY_INCOMPLETE);return;}
  if(!s->result.have_catalog){stop(s,s->result.have_delete?INVENTORY_INCOMPLETE:INVENTORY_ABSENT);return;}
  if(s->result.recorded.state==CATALOG_OPEN){stop(s,s->result.have_delete?INVENTORY_CORRUPT:INVENTORY_OPEN);return;}
  if(s->result.have_delete&&!matching_delete(s)){stop(s,INVENTORY_CORRUPT);return;}
  s->phase=I_STAT_DATA;return;
 case I_STAT_DATA:{
  bool present=false,dir=false;uint64_t n=0;if(s->ops.stat(s->ops.context,s->result.spec.path,&present,&dir,&n)!=STORE_IO_OK){stop(s,INVENTORY_IO);return;}
  if(!present){stop(s,matching_delete(s)?INVENTORY_EXPIRED:INVENTORY_MISSING);return;}
  if(dir||n!=s->result.recorded.file_bytes||s->result.have_delete){stop(s,INVENTORY_CORRUPT);return;}
  s->phase=I_STAT_LEDGER;return;
 }
 case I_STAT_LEDGER:{
  bool present=false,dir=false;uint64_t n=0;if(s->ops.stat(s->ops.context,s->result.ledger_path,&present,&dir,&n)!=STORE_IO_OK){stop(s,INVENTORY_IO);return;}
  stop(s,!present?INVENTORY_MISSING:dir||n<s->result.recorded.family_prefix?INVENTORY_CORRUPT:INVENTORY_PRESENT);return;
 }
 case I_CLEAN_FILE:
  if(s->ops.close_file(s->ops.context)!=STORE_IO_OK){s->phase=I_HELD;return;}s->file_open=false;s->phase=I_CLEAN_DIR;return;
 case I_CLEAN_DIR:
  for(unsigned k=STORAGE_INVENTORY_DEPTH;k>0;k--)if(s->dirs[k-1]){if(s->ops.close_directory(s->ops.context,k-1)!=STORE_IO_OK){s->phase=I_HELD;return;}s->dirs[k-1]=false;return;}
  s->phase=I_FINISH;return;
 case I_FINISH:
  if(s->snapshot){if(s->desired!=INVENTORY_CANCELLED&&s->ops.version(s->ops.context)!=s->result.version)s->desired=INVENTORY_INCOMPLETE;s->ops.release(s->ops.context);s->snapshot=false;}
  s->result.status=s->desired;s->phase=I_DONE;return;
 default:return;
 }
}
static bool current(const StorageInventory *s){return owned(s)&&storage_inventory_ready(s)&&s->result.version&&s->ops.version(s->ops.context)==s->result.version;}
static int cached(StorageFileHandles *h,uint64_t session,uint64_t file){for(unsigned k=0;k<STORAGE_HANDLES;k++)if(h->files[k].used&&h->files[k].spec.session_id==session&&h->files[k].spec.file_id==file)return (int)k;return -1;}
static bool same_recorded(const StorageCatalogEntry *a,const StorageCatalogEntry *b){
 return a->state==b->state&&a->descriptor.file_id==b->descriptor.file_id&&a->descriptor.role==b->descriptor.role&&a->descriptor.created_us==b->descriptor.created_us&&a->descriptor.created_known==b->descriptor.created_known&&a->descriptor.location==b->descriptor.location&&a->descriptor.ordinal==b->descriptor.ordinal&&!memcmp(a->descriptor.name,b->descriptor.name,64)&&a->revision==b->revision&&a->file_bytes==b->file_bytes&&a->family_prefix==b->family_prefix&&a->records==b->records&&a->first_seq==b->first_seq&&a->last_seq==b->last_seq&&a->sealed_us==b->sealed_us&&a->sealed_known==b->sealed_known&&a->admission_cutoff==b->admission_cutoff&&a->file_crc==b->file_crc&&a->definitions==b->definitions;
}
StorageResult storage_inventory_load_handles(const StorageInventory *s,StorageFileHandles *h){
 if(!current(s)||s->result.status!=INVENTORY_PRESENT||!h||!h->ops.is_owner||!h->ops.is_owner(h->ops.context))return STORE_INVALID;
 const StorageHandleSpec *spec=&s->result.spec;int data=cached(h,spec->session_id,spec->file_id),ledger=cached(h,spec->session_id,spec->ledger_id);
 if(data>=0){StorageHandle *e=&h->files[data];if(e->spec.kind!=HANDLE_DATA||e->spec.catalog_id!=spec->catalog_id||e->spec.ledger_id!=spec->ledger_id||e->spec.family!=spec->family||strcmp(e->spec.path,spec->path)||e->spec.order_key!=spec->order_key||!e->sealed||!same_recorded(&e->seal,&s->result.recorded))return STORE_INVALID;}
 if(ledger>=0){StorageHandle *e=&h->files[ledger];if(e->spec.kind!=HANDLE_AUXILIARY||strcmp(e->spec.path,s->result.ledger_path))return STORE_INVALID;}
 unsigned free=0;for(unsigned k=0;k<STORAGE_HANDLES;k++)if(!h->files[k].used)free++;
 if(free<(unsigned)((data<0)+(ledger<0)))return STORE_CAPACITY;
 bool created=false;
 if(data<0){unsigned k;StorageResult r=storage_file_handles_register(h,spec,&k);if(r!=STORE_OK)return r;data=(int)k;created=true;r=storage_file_handles_seal(h,k,&s->result.recorded);if(r!=STORE_OK){(void)storage_file_handles_forget(h,k);return r;}}
 if(ledger<0){StorageHandleSpec l={.session_id=spec->session_id,.file_id=spec->ledger_id,.kind=HANDLE_AUXILIARY,.family=RETAIN_METADATA};strcpy(l.path,s->result.ledger_path);unsigned k;StorageResult r=storage_file_handles_register(h,&l,&k);if(r!=STORE_OK){if(created)(void)storage_file_handles_forget(h,(unsigned)data);return r;}}
 return STORE_OK;
}
static StorageQueryOpen query_open(void *ctx,const StorageQueryRequest *r,StorageQuerySource *out){
 StorageInventoryQuery *q=ctx;if(!q||!r||!out)return QUERY_SOURCE_IO;
 StorageInventory *s=q->inventory;if(!current(s)||r->session_id!=s->result.session_id||r->file_id!=s->result.file_id)return QUERY_SOURCE_NOT_READY;
 switch(s->result.status){
 case INVENTORY_PRESENT:{StorageResult result=storage_inventory_load_handles(s,q->handles);if(result==STORE_CAPACITY||result==STORE_BUSY)return QUERY_SOURCE_NOT_READY;if(result!=STORE_OK)return QUERY_SOURCE_IO;return q->cached.open(q->cached.context,r,out);}
 case INVENTORY_ABSENT:return QUERY_SOURCE_MISSING;
 case INVENTORY_EXPIRED:return QUERY_SOURCE_EXPIRED;
 case INVENTORY_MISSING:return QUERY_SOURCE_DATA_MISSING;
 case INVENTORY_CORRUPT:return QUERY_SOURCE_CORRUPT;
 case INVENTORY_IO:return QUERY_SOURCE_IO;
 default:return QUERY_SOURCE_NOT_READY;
 }
}
static StorageIoResult query_release(void *ctx,StorageQuerySource *source){StorageInventoryQuery *q=ctx;return q?q->cached.release(q->cached.context,source):STORE_IO_ERROR;}
bool storage_inventory_query_init(StorageInventoryQuery *q,StorageInventory *s,StorageFileHandles *h,StorageQueryFiles *out){
 if(!q||!s||!h||!out)return false;
 q->inventory=s;q->handles=h;q->cached=storage_file_handles_query(h);*out=(StorageQueryFiles){q,query_open,query_release};return true;
}
