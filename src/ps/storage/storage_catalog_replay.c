#include "storage_catalog_replay.h"
#include <string.h>
static void stop(StorageCatalogReplay *r,StorageCatalogReplayState state,StorageCatalogReplayReason reason){r->state=state;r->reason=reason;}
static bool same_descriptor(const StorageFileDescriptor *a,const StorageFileDescriptor *b){
 return a->file_id==b->file_id&&a->created_us==b->created_us&&a->created_known==b->created_known&&a->role==b->role&&a->location==b->location&&a->ordinal==b->ordinal&&!memcmp(a->name,b->name,STORAGE_CATALOG_NAME);
}
bool storage_catalog_replay_init(StorageCatalogReplay *r,const StorageBackend *b,uint64_t session,uint64_t catalog,uint64_t ledger){
 if(!r||!b||!b->size||!b->read||!session||!catalog||!ledger||catalog==ledger)return false;
 StorageBackend backend=*b;memset(r,0,sizeof(*r));r->backend=backend;r->session_id=session;r->catalog_id=catalog;r->ledger_id=ledger;r->state=CATALOG_REPLAY_SIZE;return true;
}
void storage_catalog_replay_step(StorageCatalogReplay *r){
 if(!r)return;
 if(r->state==CATALOG_REPLAY_SIZE){
  uint64_t size=0;if(r->backend.size(r->backend.context,&size)!=STORE_IO_OK){stop(r,CATALOG_REPLAY_IO,CATALOG_REPLAY_BACKEND);return;}
  r->size=size;r->state=size?CATALOG_REPLAY_FRAME:CATALOG_REPLAY_DONE;return;
 }
 if(r->state!=CATALOG_REPLAY_FRAME)return;
 if(r->prefix==r->size){r->state=CATALOG_REPLAY_DONE;return;}
 if(r->size-r->prefix<STORAGE_CATALOG_FRAME){stop(r,CATALOG_REPLAY_TAIL,CATALOG_REPLAY_PARTIAL);return;}
 size_t actual=0;if(r->backend.read(r->backend.context,r->prefix,r->frame,sizeof(r->frame),&actual)!=STORE_IO_OK||actual!=sizeof(r->frame)){stop(r,CATALOG_REPLAY_IO,CATALOG_REPLAY_BACKEND);return;}
 StorageCatalogEntry e;if(!storage_file_catalog_decode(r->frame,sizeof(r->frame),r->session_id,r->catalog_id,r->ledger_id,&e)){stop(r,CATALOG_REPLAY_TAIL,CATALOG_REPLAY_FORMAT);return;}
 if(r->revision==UINT64_MAX||e.revision!=r->revision+1){stop(r,CATALOG_REPLAY_TAIL,CATALOG_REPLAY_REVISION);return;}
 unsigned k=0;for(;k<r->count;k++)if(r->files[k].descriptor.file_id==e.descriptor.file_id)break;
 if(e.state==CATALOG_OPEN){
  if(k!=r->count){stop(r,CATALOG_REPLAY_TAIL,CATALOG_REPLAY_DUPLICATE_ID);return;}
  for(unsigned i=0;i<r->count;i++)if(r->files[i].descriptor.location==e.descriptor.location&&r->files[i].descriptor.ordinal==e.descriptor.ordinal&&!strcmp(r->files[i].descriptor.name,e.descriptor.name)){stop(r,CATALOG_REPLAY_TAIL,CATALOG_REPLAY_DUPLICATE_NAME);return;}
  if(r->count==STORAGE_FAMILY_FILES){stop(r,CATALOG_REPLAY_CAPACITY,CATALOG_REPLAY_FILE_LIMIT);return;}
  r->files[r->count++]=e;
 }else{
  if(k==r->count||r->files[k].state!=CATALOG_OPEN){stop(r,CATALOG_REPLAY_TAIL,CATALOG_REPLAY_ORDER);return;}
  if(!same_descriptor(&r->files[k].descriptor,&e.descriptor)){stop(r,CATALOG_REPLAY_TAIL,CATALOG_REPLAY_DESCRIPTOR);return;}
  if(e.family_prefix<r->last_family_prefix||e.admission_cutoff<r->last_cutoff||e.records>e.family_prefix/512){stop(r,CATALOG_REPLAY_TAIL,CATALOG_REPLAY_PROOF_ORDER);return;}
  r->files[k]=e;r->last_family_prefix=e.family_prefix;r->last_cutoff=e.admission_cutoff;
 }
 r->prefix+=STORAGE_CATALOG_FRAME;r->revision=e.revision;
 if(r->prefix==r->size)r->state=CATALOG_REPLAY_DONE;
}
void storage_catalog_replay_cancel(StorageCatalogReplay *r){if(r&&(r->state==CATALOG_REPLAY_SIZE||r->state==CATALOG_REPLAY_FRAME))stop(r,CATALOG_REPLAY_CANCELLED,CATALOG_REPLAY_CANCEL);}
StorageResult storage_catalog_replay_get_recorded(const StorageCatalogReplay *r,uint64_t file,StorageCatalogHistory *out){
 if(!r||!file||!out)return STORE_INVALID;
 if(r->state==CATALOG_REPLAY_SIZE||r->state==CATALOG_REPLAY_FRAME)return STORE_BUSY;
 for(unsigned i=0;i<r->count;i++)if(r->files[i].descriptor.file_id==file){
  StorageCatalogHistory h={0};h.recorded=r->files[i];h.evidence=h.recorded.state==CATALOG_SEALED?CATALOG_RECORDED_SEAL_UNVERIFIED:CATALOG_RECORDED_OPEN;h.scan_state=r->state;h.stopped_because=r->reason;h.accepted_prefix=r->prefix;h.snapshot_bytes=r->size;*out=h;return STORE_OK;
 }
 return r->state==CATALOG_REPLAY_DONE?STORE_EXPIRED:STORE_INVALID;
}
