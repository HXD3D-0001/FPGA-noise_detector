#include "fatfs_provision_port.h"
#include <string.h>
static bool bound(LfnFatFsProvisionPort *p){return p&&p->files&&p->guard&&p->files->volume&&p->files->volume->mounted&&lfn_fatfs_namespace_port_matches(p->guard,p->files->volume,p->guard->space);}
static bool validate(void *ctx,StorageFileHandles *h,StorageNamespace *s,const unsigned *slots,unsigned n){
 LfnFatFsProvisionPort *p=ctx;if(!bound(p)||!h||h->ops.context!=p->files||s!=p->guard->space||n<3||n>STORAGE_PROVISION_FILES||!slots)return false;
 for(unsigned k=0;k<n;k++){
  if(slots[k]>=STORAGE_HANDLES||p->files->files[slots[k]].opened)return false;
  StorageHandle *e=&h->files[slots[k]];if(!e->used||!memchr(e->spec.path,0,sizeof(e->spec.path)))return false;
  const char *base=strrchr(e->spec.path,'/');base=base?base+1:e->spec.path;bool found=false;
  for(unsigned j=0;j<p->guard->count;j++)if(!strcmp(base,p->guard->names[j].name)){if(p->guard->names[j].snapshot_metadata!=(k==n-1))return false;found=true;break;}
  if(!found)return false;
 }
 return true;
}
static StorageResult create(void *ctx,StorageFileHandles *h,unsigned slot){LfnFatFsProvisionPort *p=ctx;return bound(p)?lfn_fatfs_file_port_create(p->files,h,slot):STORE_INVALID;}
static StorageIoResult sync_media(void *ctx,StorageFileHandles *h){LfnFatFsProvisionPort *p=ctx;return bound(p)&&h&&h->ops.context==p->files&&h->ops.sync_media?h->ops.sync_media(h->ops.context):STORE_IO_ERROR;}
bool lfn_fatfs_provision_port_init(LfnFatFsProvisionPort *p,LfnFatFsFilePort *f,LfnFatFsNamespacePort *g,StorageProvisionOps *out){
 LfnFatFsProvisionPort copy={f,g};if(!p||!out||!bound(&copy))return false;*p=copy;*out=(StorageProvisionOps){p,validate,create,sync_media};return true;
}
