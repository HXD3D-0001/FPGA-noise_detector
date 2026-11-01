#include "fatfs_file_port.h"
#include <string.h>
static bool owner(void *v){LfnFatFsFilePort *p=v;return p&&p->volume&&p->volume->mounted&&p->volume->is_owner&&p->volume->is_owner(p->volume->owner_context);}
static StorageIoResult open_read(void *v,unsigned k,const char *path,StorageBackend *b){
 LfnFatFsFilePort *p=v;if(!owner(p)||k>=STORAGE_HANDLES||p->files[k].opened||!b)return STORE_IO_ERROR;
 StorageIoResult r=lfn_fatfs_open(&p->files[k],p->volume,path,false,false);if(r!=STORE_IO_OK)return r;
 return lfn_fatfs_backend(&p->files[k],b)?STORE_IO_OK:STORE_IO_ERROR;
}
static StorageIoResult close_file(void *v,unsigned k){LfnFatFsFilePort *p=v;return owner(p)&&k<STORAGE_HANDLES?lfn_fatfs_close(&p->files[k]):STORE_IO_ERROR;}
static StorageIoResult remove_file(void *v,const char *path,bool *missing){LfnFatFsFilePort *p=v;return owner(p)?lfn_fatfs_remove(p->volume,path,missing):STORE_IO_ERROR;}
static StorageIoResult sync_media(void *v){LfnFatFsFilePort *p=v;return owner(p)&&(!p->volume->permit||p->volume->permit(p->volume->guard_context,NULL,true))?p->media_sync(p->sync_context):STORE_IO_ERROR;}
bool lfn_fatfs_file_port_init(LfnFatFsFilePort *p,LfnFatFsVolume *volume,StorageIoResult (*sync)(void *),void *ctx,StorageFileOps *out){
 if(!p||!volume||!volume->mounted||!volume->is_owner||!volume->is_owner(volume->owner_context)||!sync||!out)return false;
 memset(p,0,sizeof(*p));p->volume=volume;p->media_sync=sync;p->sync_context=ctx;*out=(StorageFileOps){p,owner,open_read,close_file,remove_file,sync_media};return true;
}
StorageResult lfn_fatfs_file_port_create(LfnFatFsFilePort *p,StorageFileHandles *h,unsigned k){
 if(!owner(p)||!h||h->ops.context!=p||h->ops.open!=open_read||h->ops.close!=close_file||k>=STORAGE_HANDLES)return STORE_INVALID;
 StorageHandle *e=&h->files[k];if(!e->used||e->opened||e->active||e->sealed||e->references||e->closing||e->claimed||e->removed||p->files[k].opened)return STORE_BUSY;
 StorageIoResult r=lfn_fatfs_open(&p->files[k],p->volume,e->spec.path,true,true);if(r!=STORE_IO_OK)return STORE_INVALID;
 StorageBackend b;if(!lfn_fatfs_backend(&p->files[k],&b))return STORE_INVALID;
 return storage_file_handles_attach(h,k,&b);
}
