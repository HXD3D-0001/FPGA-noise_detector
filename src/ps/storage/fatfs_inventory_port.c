#include "fatfs_inventory_port.h"
#include <string.h>
static bool owner(void *ctx){LfnFatFsInventoryPort *p=ctx;return p&&p->volume&&p->volume->mounted&&p->volume->is_owner&&p->volume->is_owner(p->volume->owner_context);}
static StorageResult acquire(void *ctx,const char *root,uint64_t *v){LfnFatFsInventoryPort *p=ctx;return owner(p)?p->guard.acquire(p->guard.context,root,v):STORE_INVALID;}
static uint64_t version(void *ctx){LfnFatFsInventoryPort *p=ctx;return owner(p)?p->guard.version(p->guard.context):0;}
static void release(void *ctx){LfnFatFsInventoryPort *p=ctx;if(owner(p))p->guard.release(p->guard.context);}
static StorageIoResult open_directory(void *ctx,unsigned depth,const char *path){
 LfnFatFsInventoryPort *p=ctx;if(!owner(p)||depth>=STORAGE_INVENTORY_DEPTH||p->directories[depth].opened)return STORE_IO_ERROR;
 return lfn_fatfs_scan_open(&p->directories[depth],p->volume,path);
}
static StorageIoResult next(void *ctx,unsigned depth,StorageDirectoryItem *out,bool *have){
 LfnFatFsInventoryPort *p=ctx;if(!owner(p)||depth>=STORAGE_INVENTORY_DEPTH||!out||!have)return STORE_IO_ERROR;
 LfnFatFsEntry item;StorageIoResult r=lfn_fatfs_scan_next(&p->directories[depth],&item,have);if(r!=STORE_IO_OK||!*have)return r;
 size_t n=strlen(item.name);if(n>=sizeof(out->name))return STORE_IO_ERROR;
 memset(out,0,sizeof(*out));memcpy(out->name,item.name,n+1);out->bytes=item.bytes;out->directory=item.directory;return STORE_IO_OK;
}
static StorageIoResult close_directory(void *ctx,unsigned depth){LfnFatFsInventoryPort *p=ctx;return owner(p)&&depth<STORAGE_INVENTORY_DEPTH?lfn_fatfs_scan_close(&p->directories[depth]):STORE_IO_ERROR;}
static StorageIoResult open_file(void *ctx,const char *path,StorageBackend *out){
 LfnFatFsInventoryPort *p=ctx;if(!owner(p)||p->metadata.opened||!out)return STORE_IO_ERROR;
 StorageIoResult r=lfn_fatfs_open(&p->metadata,p->volume,path,false,false);if(r!=STORE_IO_OK)return r;
 /* Even a violated backend contract leaves the successfully opened file owned
  * by core cleanup; returning OK with empty backend makes core close it. */
 if(!lfn_fatfs_backend(&p->metadata,out))memset(out,0,sizeof(*out));
 return STORE_IO_OK;
}
static StorageIoResult close_file(void *ctx){LfnFatFsInventoryPort *p=ctx;return owner(p)?lfn_fatfs_close(&p->metadata):STORE_IO_ERROR;}
static StorageIoResult stat_file(void *ctx,const char *path,bool *exists,bool *directory,uint64_t *bytes){LfnFatFsInventoryPort *p=ctx;return owner(p)?lfn_fatfs_stat(p->volume,path,exists,directory,bytes):STORE_IO_ERROR;}
bool lfn_fatfs_inventory_port_init(LfnFatFsInventoryPort *p,LfnFatFsVolume *v,const StorageNamespaceGuard *g,StorageInventoryOps *out){
 if(!p||!v||!v->mounted||!v->is_owner||!v->is_owner(v->owner_context)||!g||!g->acquire||!g->version||!g->release||!out)return false;
 StorageNamespaceGuard guard=*g;memset(p,0,sizeof(*p));p->volume=v;p->guard=guard;
 *out=(StorageInventoryOps){p,owner,acquire,version,release,open_directory,next,close_directory,open_file,close_file,stat_file};return true;
}
