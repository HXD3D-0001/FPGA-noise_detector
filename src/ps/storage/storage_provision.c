#include "storage_provision.h"
#include <string.h>
/* Inert identity-only stand-ins used exclusively by factory's pure validator. */
static StorageIoResult inert_size(void *v,uint64_t *n){(void)v;(void)n;return STORE_IO_ERROR;}
static StorageIoResult inert_read(void *v,uint64_t o,uint8_t *b,size_t n,size_t *a){(void)v;(void)o;(void)b;(void)n;(void)a;return STORE_IO_ERROR;}
static StorageIoResult inert_write(void *v,uint64_t o,const uint8_t *b,size_t n,size_t *a){(void)v;(void)o;(void)b;(void)n;(void)a;return STORE_IO_ERROR;}
static StorageIoResult inert_sync(void *v){(void)v;return STORE_IO_ERROR;}
static bool empty_backend(const StorageBackend *b){return !b->context&&!b->size&&!b->read&&!b->write&&!b->sync&&!b->truncate&&!b->validate;}
static bool owned(StorageProvision *p){return p&&p->space&&p->space->is_owner&&p->space->is_owner(p->space->context)&&p->config.is_owner&&p->config.is_owner(p->config.owner_context);}
static bool layout(const StorageFactoryConfig *c,StorageFileHandles *h,StorageNamespace *s,const unsigned *slots){
 size_t root=strlen(s->root),parent=0;const char *first=NULL;
 for(unsigned k=0;k<c->count+2;k++){
  unsigned n=slots[k];if(n>=STORAGE_HANDLES)return false;for(unsigned j=0;j<k;j++)if(slots[j]==n)return false;
  StorageHandle *e=&h->files[n];if(!e->used||e->opened||e->active||e->sealed||e->references||e->closing||e->claimed||e->removed||e->expired||!memchr(e->spec.path,0,sizeof(e->spec.path)))return false;
  StorageHandleSpec *a=&e->spec;if(a->session_id!=c->session_id||strncmp(a->path,s->root,root)||a->path[root]!='/')return false;
  const char *base=strrchr(a->path,'/');if(!base||!base[1])return false;size_t len=(size_t)(base-a->path);
  if(first){if(parent!=len||memcmp(first,a->path,len))return false;}else{first=a->path;parent=len;}
  uint64_t id=k<c->count?c->files[k].descriptor.file_id:k==c->count?c->ledger_id:c->catalog_id;
  if(a->file_id!=id)return false;
  if(k<c->count){if(a->kind!=HANDLE_DATA||a->catalog_id!=c->catalog_id||a->ledger_id!=c->ledger_id||!memchr(c->files[k].descriptor.name,0,64)||strcmp(base+1,c->files[k].descriptor.name))return false;}
  else if(a->kind!=HANDLE_AUXILIARY)return false;
 }
 return true;
}
typedef struct {uintptr_t first,last;} ProvisionRegion;
static bool away(const ProvisionRegion *r,const void *ptr,size_t bytes){
 if(!ptr||(uintptr_t)ptr>UINTPTR_MAX-bytes)return false;
 uintptr_t first=(uintptr_t)ptr,last=first+bytes;
 for(unsigned k=0;k<4;k++)if(first<r[k].last&&r[k].first<last)return false;
 return true;
}
static bool geometry(StorageProvision *p,StorageFileHandles *h,StorageNamespace *s,StorageFactory *f,StorageLifecycle *l,const StorageFactoryConfig *c){
 const void *ptrs[]={p,h,s,l};size_t sizes[]={sizeof(*p),sizeof(*h),sizeof(*s),sizeof(*l)};ProvisionRegion r[4];
 for(unsigned k=0;k<4;k++){if((uintptr_t)ptrs[k]>UINTPTR_MAX-sizes[k])return false;r[k]=(ProvisionRegion){(uintptr_t)ptrs[k],(uintptr_t)ptrs[k]+sizes[k]};for(unsigned j=0;j<k;j++)if(r[k].first<r[j].last&&r[j].first<r[k].last)return false;}
#define AWAY(x) if(!away(r,(x),sizeof(*(x))))return false
 AWAY(f);AWAY(c->pool);AWAY(c->family);AWAY(c->owner);AWAY(c->catalog);AWAY(c->verifier);AWAY(c->epoch);AWAY(c->engine);
 if(!away(r,c->arena,STORAGE_POOL_BYTES))return false;
 for(unsigned k=0;k<c->count;k++){const StorageFactoryFile *a=&c->files[k];AWAY(a->binding);AWAY(a->writer);AWAY(a->queue);AWAY(a->service);AWAY(a->pump);AWAY(a->plan);}
#undef AWAY
 return true;
}
bool storage_provision_init(StorageProvision *p,const StorageProvisionOps *o,StorageFileHandles *h,StorageNamespace *s,StorageFactory *f,StorageLifecycle *l,const StorageFactoryConfig *c,const unsigned *slots){
 if(!p||!o||!o->validate||!o->create||!o->sync_media||!h||!s||!f||!l||!c||!slots||!c->count||c->count>STORAGE_FAMILY_FILES||!storage_namespace_version(s)||s->writing||!h->ops.is_owner||!h->ops.is_owner(h->ops.context)||!geometry(p,h,s,f,l,c)||!empty_backend(&c->ledger)||!empty_backend(&c->manifest)||!layout(c,h,s,slots)||!o->validate(o->context,h,s,slots,c->count+2))return false;
 StorageFactoryConfig copy=*c;StorageProvisionOps ops=*o;unsigned order[STORAGE_PROVISION_FILES]={0};
 for(unsigned k=0;k<c->count+2;k++){order[k]=slots[k];StorageBackend b={&h->files[slots[k]],inert_size,inert_read,inert_write,inert_sync,NULL,NULL};if(k<c->count){if(!empty_backend(&c->files[k].raw))return false;copy.files[k].raw=b;}else if(k==c->count)copy.ledger=b;else copy.manifest=b;}
 if(!storage_factory_validate(f,&copy))return false;
 memset(p,0,sizeof(*p));p->config=copy;p->ops=ops;p->handles=h;p->space=s;p->factory=f;p->lifecycle=l;memcpy(p->slots,order,sizeof(order));return true;
}
static void fail(StorageProvision *p,StorageResult r){p->error=r;p->failed_at=p->state;p->state=PROVISION_FAILED;}
static StorageBackend *backend(StorageProvision *p,unsigned k){return k<p->config.count?&p->config.files[k].raw:k==p->config.count?&p->config.ledger:&p->config.manifest;}
void storage_provision_step(StorageProvision *p,uint64_t now){
 if(!owned(p)||p->state>=PROVISION_METADATA)return;
 if(p->have_time&&now<p->last_now){if(p->state==PROVISION_ASSEMBLE)p->lifecycle->state=LIFECYCLE_FAILED;fail(p,STORE_INVALID);return;}p->have_time=true;p->last_now=now;
 if(!p->config.hooks.quiescent(p->config.hooks.context)){if(p->state==PROVISION_ASSEMBLE)p->lifecycle->state=LIFECYCLE_FAILED;fail(p,STORE_BUSY);return;}
 unsigned total=p->config.count+2;
 if(p->state==PROVISION_WAIT_WRITE){StorageResult r=storage_namespace_write_begin(p->space,&p->lease);if(r==STORE_BUSY)return;if(r!=STORE_OK){fail(p,r);return;}p->state=PROVISION_CREATE;return;}
 if(p->state==PROVISION_CREATE){
  StorageResult r=p->ops.create(p->ops.context,p->handles,p->slots[p->cursor]);if(r==STORE_BUSY)return;if(r!=STORE_OK){fail(p,r);return;}
  StorageHandle *e=&p->handles->files[p->slots[p->cursor]];StorageBackend *b=&e->backend;
  if(!e->opened||!e->active||!b->size||!b->read||!b->write||!b->sync){fail(p,STORE_INVALID);return;}*backend(p,p->cursor)=*b;p->created++;
  if(++p->cursor==total){p->cursor=0;p->state=PROVISION_SYNC_FILES;}return;
 }
 if(p->state==PROVISION_SYNC_FILES){StorageBackend *b=backend(p,p->cursor);StorageIoResult r=b->sync(b->context);if(r!=STORE_IO_OK){p->io_result=r;p->have_io_result=true;fail(p,STORE_INVALID);return;}if(++p->cursor==total)p->state=PROVISION_SYNC_MEDIA;return;}
 if(p->state==PROVISION_SYNC_MEDIA){StorageIoResult r=p->ops.sync_media(p->ops.context,p->handles);if(r!=STORE_IO_OK){p->io_result=r;p->have_io_result=true;fail(p,STORE_INVALID);return;}p->state=PROVISION_HANDOFF;return;}
 if(p->state==PROVISION_HANDOFF){
  if(!storage_factory_init(p->factory,&p->config)||!storage_lifecycle_init(p->lifecycle,p->factory,p->space)||storage_lifecycle_adopt_write(p->lifecycle,p->lease)!=STORE_OK){fail(p,STORE_INVALID);return;}
  p->lease=0;p->state=PROVISION_ASSEMBLE;return;
 }
 if(p->state==PROVISION_ASSEMBLE){storage_lifecycle_step(p->lifecycle,now);if(p->lifecycle->state==LIFECYCLE_FAILED)fail(p,STORE_INVALID);else if(p->lifecycle->state==LIFECYCLE_METADATA)p->state=PROVISION_METADATA;}
}
