#include "storage_owner_freertos.h"
#include <string.h>
static bool handles_quiet(StorageOwnerFreertos *f){
 for(unsigned k=0;k<STORAGE_HANDLES;k++){
  StorageHandle *h=&f->owner->workspace.handles.files[k];
  if(h->used&&(h->opened||h->closing||h->claimed))return false;
 }
 return true;
}
bool lfn_storage_owner_freertos_mount(StorageOwnerFreertos *f,StorageOwner *o,const StorageOwnerFreertosConfig *c,const StorageOwnerConfig *oc){
 if(!f||!o||!c||!oc||!c->volume||!c->drive||!c->names||!c->name_count)return false;
 if(!c->media_sync||!c->is_owner||!c->is_owner(c->owner_context)||!c->producer_stopped)return false;
 if(!c->producer_stopped(c->proof_context))return false;
 memset(f,0,sizeof(*f));f->volume=c->volume;f->owner=o;
 f->stage=1;
 if(!lfn_fatfs_volume_init(f->volume,c->drive,c->is_owner,c->owner_context))return false;
 f->stage=2;
 if(!storage_owner_prepare(o,oc))return false;
 f->stage=3;
 if(!lfn_fatfs_namespace_port_bind(&f->guard,f->volume,&o->workspace.space,c->names,c->name_count))return false;
 uint64_t lease=0;
 if(storage_namespace_write_begin(&o->workspace.space,&lease)!=STORE_OK)return false;
 StorageIoResult mounted=lfn_fatfs_mount(f->volume);
 StorageResult ended=storage_namespace_write_end(&o->workspace.space,lease);
 if(mounted!=STORE_IO_OK||ended!=STORE_OK){f->stage=4;return false;}
 f->stage=5;
 if(!lfn_fatfs_file_port_init(&f->files,f->volume,c->media_sync,c->media_context,&f->file_ops))return false;
 f->stage=6;
 if(!lfn_fatfs_provision_port_init(&f->provision,&f->files,&f->guard,&f->provision_ops))return false;
 f->ports=(StorageOwnerPorts){f->file_ops,f->provision_ops,c->cold,c->rotation,
  c->is_owner,c->owner_context,c->producer_stopped,c->proof_context};
 f->stage=7;
 if(!storage_owner_attach_ports(o,&f->ports))return false;
 f->stage=100;return true;
}
StorageIoResult lfn_storage_owner_freertos_unmount(StorageOwnerFreertos *f){
 if(!f||!f->volume||!f->owner)return STORE_IO_ERROR;
 if(!handles_quiet(f))return STORE_IO_ERROR;
 if(!f->volume->mounted)return STORE_IO_OK;
 uint64_t lease=0;
 if(storage_namespace_write_begin(&f->owner->workspace.space,&lease)!=STORE_OK)return STORE_IO_ERROR;
 StorageIoResult r=lfn_fatfs_unmount(f->volume);
 if(storage_namespace_write_end(&f->owner->workspace.space,lease)!=STORE_OK&&r==STORE_IO_OK)r=STORE_IO_ERROR;
 return r;
}
bool lfn_storage_owner_freertos_mounted(const StorageOwnerFreertos *f){
 return f&&f->volume&&f->volume->mounted;
}