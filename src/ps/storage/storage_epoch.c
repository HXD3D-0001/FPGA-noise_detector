#include "storage_epoch.h"
#include <string.h>
static void fail(StorageEpoch *e,StorageResult r){e->state=EPOCH_FAILED;e->error=r;}
bool storage_epoch_init(StorageEpoch *e,StorageFileCatalog *c,StorageBootstrapPlan **plans,const uint32_t *config,const uint64_t *capture,const StorageEpochHooks *hooks){
 if(!e||!c||!plans||!config||!capture||!hooks||!hooks->quiescent||!hooks->close||c->phase!=CATALOG_IDLE||c->epoch_handoff||!hooks->quiescent(hooks->context))return false;
 for(unsigned k=0;k<c->count;k++)if(!plans[k]||!c->services[k]->health.sealed||c->services[k]->health.fatal||!config[k]||!capture[k])return false;
 StorageEpochHooks h=*hooks;memset(e,0,sizeof(*e));e->catalog=c;e->hooks=h;
 for(unsigned k=0;k<c->count;k++){e->plans[k]=plans[k];e->config[k]=config[k];e->capture[k]=capture[k];}
 c->epoch_handoff=true;e->state=EPOCH_DRAIN;return true;
}
void storage_epoch_step(StorageEpoch *e,uint64_t now){
 if(!e||e->state>=EPOCH_READY)return;
 StorageFileCatalog *c=e->catalog;StorageFamilyQueue *owner=c->owner;
 if(!e->hooks.quiescent(e->hooks.context)||c->phase==CATALOG_FAULT||owner->family->phase==FAMILY_FAULT){fail(e,STORE_INVALID);return;}
 for(unsigned k=0;k<c->count;k++)if(c->services[k]->health.fatal){fail(e,STORE_INVALID);return;}
 if(e->state==EPOCH_DRAIN){
  bool drained=true;for(unsigned k=0;k<c->count;k++)if(!c->services[k]->health.drained)drained=false;
  if(!drained){storage_service_step(c->services[e->round_robin++%c->count],now);return;}
  e->state=EPOCH_SEAL;e->cursor=0;return;
 }
 if(e->state==EPOCH_SEAL){
  if(c->phase!=CATALOG_IDLE){storage_file_catalog_step(c,now);return;}
  if(e->cursor==c->count){e->cursor=0;e->state=EPOCH_COPY;return;}
  if(c->files[e->cursor].state==CATALOG_SEALED){e->cursor++;return;}
  StorageResult r=storage_file_catalog_request_seal(c,c->files[e->cursor].descriptor.file_id,0,false);
  if(r!=STORE_OK&&r!=STORE_BUSY)fail(e,r);
  return;
 }
 if(e->state==EPOCH_COPY){
  if(e->cursor==c->count){e->cursor=e->slot=0;e->state=EPOCH_RELEASE;return;}
  unsigned k=e->cursor;StorageResult r=storage_bootstrap_plan(e->plans[k],c->services[k],owner->family,c->files[k].descriptor.file_id,e->config[k],e->capture[k]);
  if(r!=STORE_OK){fail(e,r);return;}e->cursor++;return;
 }
 if(e->state==EPOCH_RELEASE){
  if(e->cursor==c->count){e->cursor=0;e->state=EPOCH_ACK_BARRIER;return;}
  StorageService *s=c->services[e->cursor];
  if(e->slot==STORAGE_RECEIPTS){e->cursor++;e->slot=0;return;}
  StorageServiceSlot *slot=&s->slots[e->slot];if(slot->occupied){StorageResult r=storage_service_release(s,slot->ticket);if(r==STORE_BUSY)return;if(r!=STORE_OK){fail(e,r);return;}}
  e->slot++;return;
 }
 if(e->state==EPOCH_ACK_BARRIER){
  for(unsigned k=0;k<c->count;k++){StorageQueueHealth h;StorageResult r=storage_queue_health(c->services[k]->queue,&h);if(r==STORE_BUSY)return;if(r!=STORE_OK||h.live_jobs||!h.input_closed){fail(e,STORE_INVALID);return;}if(h.retained_receipts)return;}
  e->cursor=0;e->state=EPOCH_CLOSE;return;
 }
 if(e->state==EPOCH_CLOSE){
  uint64_t id=e->cursor<c->count?c->files[e->cursor].descriptor.file_id:e->cursor==c->count?owner->family->ledger_id:c->catalog_id;
  StorageResult r=e->hooks.close(e->hooks.context,id);if(r==STORE_BUSY)return;if(r!=STORE_OK){fail(e,r);return;}
  if(++e->cursor<c->count+2)return;
  e->token.session_id=owner->sequence.session_id;e->token.next_sequence=owner->sequence.next_seq;e->token.catalog_id=c->catalog_id;e->token.ledger_id=owner->family->ledger_id;e->token.count=c->count;
  for(unsigned k=0;k<c->count;k++)e->token.files[k]=c->files[k].descriptor.file_id;
  e->token.valid=true;e->state=EPOCH_READY;
 }
}
bool storage_epoch_consume(StorageEpoch *e,uint64_t catalog,uint64_t ledger,const uint64_t *files,unsigned count,uint64_t *session,uint64_t *seed){
 if(!e||e->state!=EPOCH_READY||!e->token.valid||e->token.consumed||!catalog||!ledger||catalog==ledger||!files||!count||count>STORAGE_FAMILY_FILES||!session||!seed||e->token.next_sequence==UINT64_MAX)return false;
 uint64_t old[STORAGE_FAMILY_FILES+2];old[0]=e->token.catalog_id;old[1]=e->token.ledger_id;for(unsigned k=0;k<e->token.count;k++)old[k+2]=e->token.files[k];
 for(unsigned k=0;k<e->token.count+2;k++){if(catalog==old[k]||ledger==old[k])return false;for(unsigned j=0;j<count;j++)if(files[j]==old[k])return false;}
 for(unsigned k=0;k<count;k++){if(!files[k]||files[k]==catalog||files[k]==ledger)return false;for(unsigned j=0;j<k;j++)if(files[k]==files[j])return false;}
 *session=e->token.session_id;*seed=e->token.next_sequence;e->token.consumed=true;return true;
}
