#include "storage_file_catalog.h"
#include "protocol.h"
#include <string.h>
#include <stdio.h>
static void put(uint8_t *b,uint64_t x,unsigned n){for(unsigned i=0;i<n;i++)b[i]=(uint8_t)(x>>(8*i));}
static uint64_t get(const uint8_t *b,unsigned n){uint64_t x=0;for(unsigned i=0;i<n;i++)x|=(uint64_t)b[i]<<(8*i);return x;}
static bool name_valid(const char *s){
 unsigned n=0;while(n<STORAGE_CATALOG_NAME&&s[n]){
  unsigned char c=(unsigned char)s[n];if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='_'||c=='-'))return false;n++;
 }
 if(!n||n==STORAGE_CATALOG_NAME||!strcmp(s,".")||!strcmp(s,".."))return false;
 for(unsigned i=n;i<STORAGE_CATALOG_NAME;i++)if(s[i])return false;
 return true;
}
static bool descriptor_valid(const StorageFileDescriptor *d){return d&&d->file_id&&d->role>=CATALOG_MONITOR&&d->role<=CATALOG_EVENT&&(d->created_known||!d->created_us)&&name_valid(d->name)&&((d->location==CATALOG_BASE&&!d->ordinal)||(d->location==CATALOG_EVENT_TREE&&d->role==CATALOG_EVENT&&d->ordinal&&d->ordinal<(UINT64_C(1)<<63)));}
static bool definition(RecordType t){return t==RECORD_SESSION||t==RECORD_CONFIG||t==RECORD_CAPTURE||t==RECORD_CALIBRATION||t==RECORD_TIME_MAP;}
static void encode(StorageFileCatalog *c,bool seal){
 const StorageCatalogEntry *e=&c->files[c->active];uint8_t *b=c->frame;memset(b,0,256);memcpy(b,"SFC1",4);put(b+4,e->descriptor.location==CATALOG_BASE?1:2,2);put(b+6,seal?2:1,2);
 put(b+8,c->owner->family->session_id,8);put(b+16,c->catalog_id,8);put(b+24,c->owner->family->ledger_id,8);put(b+32,e->descriptor.file_id,8);put(b+40,c->revision+1,8);
 if(seal){put(b+48,e->file_bytes,8);put(b+56,e->family_prefix,8);put(b+64,e->records,8);put(b+72,e->first_seq,8);put(b+80,e->last_seq,8);put(b+96,e->sealed_us,8);put(b+176,e->file_crc,4);put(b+180,e->definitions,4);put(b+184,e->admission_cutoff,8);}
 put(b+88,e->descriptor.created_us,8);put(b+104,e->descriptor.role,4);
 put(b+108,(e->descriptor.created_known?1:0)|(seal?4:0)|(seal&&e->sealed_known?2:0)|(seal&&!e->records?8:0),4);memcpy(b+112,e->descriptor.name,64);put(b+192,e->descriptor.location,4);put(b+200,e->descriptor.ordinal,8);put(b+252,crc32c(b,252),4);
}
bool storage_file_catalog_decode(const uint8_t *b,size_t n,uint64_t session,uint64_t catalog,uint64_t ledger,StorageCatalogEntry *out){
 if(!b||!out||n!=256||!session||!catalog||!ledger||catalog==ledger||memcmp(b,"SFC1",4)||(get(b+4,2)!=1&&get(b+4,2)!=2)||get(b+8,8)!=session||get(b+16,8)!=catalog||get(b+24,8)!=ledger||get(b+252,4)!=crc32c(b,252))return false;
 unsigned type=(unsigned)get(b+6,2),flags=(unsigned)get(b+108,4);if((type!=1&&type!=2)||(flags&~15u))return false;
 unsigned version=(unsigned)get(b+4,2);for(unsigned i=version==1?192:208;i<252;i++)if(b[i])return false;
 if(version==2)for(unsigned i=196;i<200;i++)if(b[i])return false;
 StorageCatalogEntry e={0};e.descriptor.file_id=get(b+32,8);e.revision=get(b+40,8);e.descriptor.created_us=get(b+88,8);e.descriptor.created_known=(flags&1)!=0;e.descriptor.role=(StorageCatalogRole)get(b+104,4);memcpy(e.descriptor.name,b+112,64);
 if(version==2){e.descriptor.location=(StorageCatalogLocation)get(b+192,4);e.descriptor.ordinal=get(b+200,8);if(e.descriptor.location!=CATALOG_EVENT_TREE)return false;}
 if(!descriptor_valid(&e.descriptor)||e.descriptor.file_id==catalog||e.descriptor.file_id==ledger||!e.revision)return false;
 e.file_bytes=get(b+48,8);e.family_prefix=get(b+56,8);e.records=get(b+64,8);e.first_seq=get(b+72,8);e.last_seq=get(b+80,8);e.sealed_us=get(b+96,8);e.file_crc=(uint32_t)get(b+176,4);e.definitions=(uint32_t)get(b+180,4);e.admission_cutoff=get(b+184,8);e.sealed_known=(flags&2)!=0;
 if(type==1){if((flags&14)||e.file_bytes||e.family_prefix||e.records||e.first_seq||e.last_seq||e.sealed_us||e.file_crc||e.definitions||e.admission_cutoff)return false;e.state=CATALOG_OPEN;}
 else {
  if(!(flags&4)||(!e.sealed_known&&e.sealed_us)||(e.sealed_known&&e.descriptor.created_known&&e.sealed_us<e.descriptor.created_us)||e.family_prefix%512||!e.admission_cutoff||e.definitions>STORAGE_FAMILY_ENTRIES||e.definitions>e.records)return false;
  if(!e.records){if(!(flags&8)||e.file_bytes||e.first_seq||e.last_seq||e.file_crc||e.definitions)return false;}
  else if((flags&8)||e.file_bytes<80||e.records>e.file_bytes/80||!e.first_seq||e.first_seq>e.last_seq||e.last_seq>=e.admission_cutoff||!e.family_prefix||!e.definitions)return false;
  e.state=CATALOG_SEALED;
 }
 *out=e;return true;
}
static void fault(StorageFileCatalog *c,StorageError error){c->error=error;c->phase=CATALOG_FAULT;}
static bool io_ok(StorageFileCatalog *c,StorageIoResult r){if(r==STORE_IO_OK)return true;fault(c,r==STORE_IO_NO_MEDIA?STORE_ERROR_NO_MEDIA:r==STORE_IO_NO_SPACE?STORE_ERROR_NO_SPACE:STORE_ERROR_IO);return false;}
StorageResult storage_file_catalog_init(StorageFileCatalog *c,StorageFamilyQueue *o,StorageService **services,const StorageFileDescriptor *ds,unsigned n,const StorageBackend *b,uint64_t id,StorageFileVerifier *v){
 if(!c||!o||!o->family||!o->streaming||!services||!ds||!n||n!=o->count||n!=o->family->file_count||!b||!b->size||!b->read||!b->write||!b->sync||!id||id==o->family->ledger_id||!v||o->family->closed_files||o->family->total_records||o->family->count||o->family->phase!=FAMILY_READY)return STORE_INVALID;
 StorageQueue *qs[STORAGE_FAMILY_FILES];
 if(n>STORAGE_FAMILY_FILES||(b->context==o->family->ledger.context&&b->write==o->family->ledger.write))return STORE_INVALID;
 for(unsigned i=0;i<n;i++){
  qs[i]=o->ports[i].queue;StorageWriter *w=o->family->files[o->ports[i].file];
  if(!qs[i]||!services[i]||services[i]->queue!=qs[i]||!services[i]->registry||services[i]->health.sealed||services[i]->health.fatal||atomic_load(&services[i]->registry->published)||!descriptor_valid(&ds[i])||ds[i].file_id!=w->file_id||id==w->file_id||w->prefix||(b->context==w->backend.context&&b->write==w->backend.write))return STORE_INVALID;
  for(unsigned j=0;j<i;j++)if(!strcmp(ds[i].name,ds[j].name)||ds[i].file_id==ds[j].file_id)return STORE_INVALID;
 }
 StorageResult r=storage_queue_pause_pristine(qs,n);if(r!=STORE_OK)return r;
 memset(c,0,sizeof(*c));c->backend=*b;c->owner=o;c->catalog_id=id;c->count=n;c->verifier=v;c->phase=CATALOG_SIZE;
 for(unsigned i=0;i<n;i++){c->services[i]=services[i];c->files[i].descriptor=ds[i];}
 return STORE_OK;
}
StorageResult storage_file_catalog_get(const StorageFileCatalog *c,uint64_t file,StorageCatalogEntry *out){
 if(!c||!out)return STORE_INVALID;
 for(unsigned i=0;i<c->count;i++)if(c->files[i].descriptor.file_id==file){
  if(c->files[i].state==CATALOG_PENDING)return STORE_BUSY;
  /* Never expose in-progress proof fields as a durable OPEN certificate. */
  if(c->files[i].state==CATALOG_OPEN&&c->phase!=CATALOG_IDLE&&c->active==i){StorageCatalogEntry e={0};e.descriptor=c->files[i].descriptor;e.state=CATALOG_OPEN;e.revision=c->files[i].revision;*out=e;}
  else *out=c->files[i];
  return STORE_OK;
 }
 for(unsigned i=0;i<c->history_count;i++)if(c->history[i].descriptor.file_id==file){*out=c->history[i];return STORE_OK;}
 return STORE_EXPIRED;
}
StorageResult storage_file_catalog_request_seal(StorageFileCatalog *c,uint64_t file,uint64_t time,bool known){
 if(!c||(!known&&time))return STORE_INVALID;
 if(c->phase!=CATALOG_IDLE)return STORE_BUSY;
 for(unsigned i=0;i<c->count;i++)if(c->files[i].descriptor.file_id==file){
  StorageCatalogEntry *e=&c->files[i];StorageService *s=c->services[i];
  if(e->state!=CATALOG_OPEN||!s->health.sealed||s->health.fatal||s->health.pending||s->pcm_pending)return STORE_BUSY;
  if(known&&e->descriptor.created_known&&time<e->descriptor.created_us)return STORE_INVALID;
  c->active=i;e->sealed_us=time;e->sealed_known=known;c->phase=CATALOG_DRAIN;return STORE_OK;
 }
 return STORE_EXPIRED;
}
void storage_file_catalog_step(StorageFileCatalog *c,uint64_t now){
 if(!c||c->phase==CATALOG_FAULT)return;
 if(c->have_time&&now<c->last_now){fault(c,STORE_ERROR_CLOCK);return;}c->have_time=true;c->last_now=now;
 StorageCatalogEntry *e=&c->files[c->active];StorageService *s=c->services[c->active];StorageWriter *w=s->queue->writer;StorageFileVerifier *v=c->verifier;size_t actual=0;
 switch(c->phase){
 case CATALOG_SIZE:{uint64_t n=0;if(!io_ok(c,c->backend.size(c->backend.context,&n)))return;if(n){fault(c,STORE_ERROR_DEPENDENCY);return;}encode(c,false);c->phase=CATALOG_OPEN_WRITE;break;}
 case CATALOG_OPEN_WRITE:case CATALOG_SEAL_WRITE:
  if(c->bytes>UINT64_MAX-256||c->revision==UINT64_MAX){fault(c,STORE_ERROR_DEPENDENCY);return;}
  if(!io_ok(c,c->backend.write(c->backend.context,c->bytes,c->frame,256,&actual)))return;
  if(actual!=256){fault(c,STORE_ERROR_SHORT_WRITE);return;}c->bytes+=256;c->phase=c->phase==CATALOG_OPEN_WRITE?CATALOG_OPEN_SYNC:CATALOG_SEAL_SYNC;break;
 case CATALOG_OPEN_SYNC:
  if(!io_ok(c,c->backend.sync(c->backend.context)))return;
  e->revision=++c->revision;e->state=CATALOG_OPEN;c->phase=CATALOG_OPEN_PUBLISH;break;
 case CATALOG_OPEN_PUBLISH:
  if(storage_queue_resume_pristine(s->queue)!=STORE_OK)return;
  if(c->replacing){c->replacing=false;c->active=0;c->phase=CATALOG_IDLE;break;}
  if(++c->active==c->count){c->active=0;c->phase=CATALOG_IDLE;}
  else {encode(c,false);c->phase=CATALOG_OPEN_WRITE;}break;
 case CATALOG_DRAIN:
  storage_service_step(s,now);if(s->health.fatal||c->owner->family->phase==FAMILY_FAULT){fault(c,STORE_ERROR_DEPENDENCY);return;}
  if(s->health.drained)c->phase=CATALOG_FREEZE;
  break;
 case CATALOG_FREEZE:{
  StorageQueueHealth h;if(storage_queue_close_empty(s->queue,&h)!=STORE_OK)return;
  StorageResult r=storage_family_freeze_file(c->owner->family,e->descriptor.file_id);if(r==STORE_BUSY)return;if(r!=STORE_OK){fault(c,STORE_ERROR_DEPENDENCY);return;}
  e->file_bytes=w->prefix;e->family_prefix=c->owner->family->durable_bytes;e->admission_cutoff=h.next_seq;
  storage_registry_init(&v->registry);v->count=0;c->offset=0;c->index_started=false;c->phase=CATALOG_FILE_SIZE;break;}
 case CATALOG_FILE_SIZE:{uint64_t n=0;if(!io_ok(c,w->backend.size(w->backend.context,&n)))return;if(n!=e->file_bytes){fault(c,STORE_ERROR_DEPENDENCY);return;}c->phase=CATALOG_HEADER;break;}
 case CATALOG_HEADER:
  if(c->offset==e->file_bytes){
   if(!c->index_started&&!storage_family_index_init(&c->index,&c->owner->family->ledger,c->owner->family->session_id,c->owner->family->ledger_id,e->family_prefix,1)){fault(c,STORE_ERROR_DEPENDENCY);return;}
   if(!storage_family_index_drain(&c->index)){fault(c,STORE_ERROR_DEPENDENCY);return;}
   c->index_started=true;c->phase=CATALOG_LEDGER_REST;break;
  }
  if(e->file_bytes-c->offset<80){fault(c,STORE_ERROR_DEPENDENCY);return;}
  if(!io_ok(c,w->backend.read(w->backend.context,c->offset,v->record,64,&actual)))return;
  if(actual!=64){fault(c,STORE_ERROR_SHORT_READ);return;}
  if(memcmp(v->record,"LFR1",4)||get(v->record+4,2)!=1||get(v->record+8,2)!=64||get(v->record+12,4)>RECORD_MAX_BYTES-80){fault(c,STORE_ERROR_DEPENDENCY);return;}
  c->total=(size_t)get(v->record+12,4)+80;if(c->total>e->file_bytes-c->offset){fault(c,STORE_ERROR_DEPENDENCY);return;}c->used=64;c->phase=CATALOG_BODY;break;
 case CATALOG_BODY:{
  size_t n=c->total-c->used;if(n>STORAGE_IO_CHUNK)n=STORAGE_IO_CHUNK;
  if(!io_ok(c,w->backend.read(w->backend.context,c->offset+c->used,v->record+c->used,n,&actual)))return;
  if(actual!=n){fault(c,STORE_ERROR_SHORT_READ);return;}c->used+=n;
  if(c->used==c->total){RecordView view;if(!record_decode(v->record,c->total,&view)||view.identity.session_id!=c->owner->family->session_id||view.identity.record_seq<=e->last_seq||!storage_registry_validate(&v->registry,&view.identity,view.payload,view.payload_bytes)){fault(c,STORE_ERROR_DEPENDENCY);return;}bool indexed=c->index_started?storage_family_index_next(&c->index,view.identity.record_seq):storage_family_index_init(&c->index,&c->owner->family->ledger,c->owner->family->session_id,c->owner->family->ledger_id,e->family_prefix,view.identity.record_seq);if(!indexed){fault(c,STORE_ERROR_DEPENDENCY);return;}c->index_started=true;c->phase=CATALOG_ASSOCIATION;}
  break;}
 case CATALOG_ASSOCIATION:{
  storage_family_index_step(&c->index);StorageFamilyEntry association;StorageResult r=storage_family_index_result(&c->index,&association);if(r==STORE_BUSY)return;
  if(r!=STORE_OK||storage_family_index_check_record(&c->index,e->descriptor.file_id,c->offset,v->record,c->total,storage_registry_validate,&v->registry)!=STORE_OK){fault(c,STORE_ERROR_DEPENDENCY);return;}
  for(unsigned i=0;i<association.dependency_count;i++){bool found=false;for(unsigned j=0;j<v->count;j++)if(v->definitions[j]==association.dependencies[i])found=true;if(!found){fault(c,STORE_ERROR_DEPENDENCY);return;}}
  RecordView view;if(!record_decode(v->record,c->total,&view)){fault(c,STORE_ERROR_DEPENDENCY);return;}
  if(definition(view.identity.type)){
   if(v->count==STORAGE_FAMILY_ENTRIES||storage_registry_apply(&v->registry,&view.identity,view.payload,view.payload_bytes,&v->scratch)!=META_OK){fault(c,STORE_ERROR_DEPENDENCY);return;}
   v->definitions[v->count++]=view.identity.record_seq;e->definitions++;
  }
  if(!e->records)e->first_seq=view.identity.record_seq;
  e->last_seq=view.identity.record_seq;e->records++;e->file_crc=crc32c_extend(e->file_crc,v->record,c->total);c->offset+=c->total;c->phase=CATALOG_HEADER;break;}
 case CATALOG_LEDGER_REST:{
  storage_family_index_step(&c->index);if(c->index.state==FAMILY_INDEX_SIZE||c->index.state==FAMILY_INDEX_RESERVE||c->index.state==FAMILY_INDEX_TERMINAL)return;
  uint64_t end=0;for(unsigned k=0;k<c->index.file_count;k++)if(c->index.files[k]==e->descriptor.file_id)end=c->index.ends[k];
  if(c->index.state!=FAMILY_INDEX_NOT_FOUND||end!=e->file_bytes||c->index.last_seq>=e->admission_cutoff){fault(c,STORE_ERROR_DEPENDENCY);return;}
  encode(c,true);StorageCatalogEntry decoded;if(!storage_file_catalog_decode(c->frame,256,c->owner->family->session_id,c->catalog_id,c->owner->family->ledger_id,&decoded)){fault(c,STORE_ERROR_DEPENDENCY);return;}c->phase=CATALOG_SEAL_WRITE;break;}
 case CATALOG_SEAL_SYNC:
  if(!io_ok(c,c->backend.sync(c->backend.context)))return;
  e->revision=++c->revision;e->state=CATALOG_SEALED;c->phase=CATALOG_IDLE;break;
 case CATALOG_IDLE:case CATALOG_FAULT:break;
 }
}
StorageResult storage_file_catalog_replace_event(StorageFileCatalog *c,unsigned slot,const StorageFileDescriptor *d){
 if(!c||slot>=c->count||!descriptor_valid(d)||d->role!=CATALOG_EVENT||d->location!=CATALOG_EVENT_TREE)return STORE_INVALID;
 if(c->phase!=CATALOG_IDLE||c->files[slot].state!=CATALOG_SEALED||c->files[slot].descriptor.role!=CATALOG_EVENT)return STORE_BUSY;
 if(c->count+c->history_count>=STORAGE_FAMILY_FILES)return STORE_CAPACITY;
 for(unsigned k=0;k<c->count;k++)if(c->files[k].descriptor.file_id==d->file_id)return STORE_INVALID;
 for(unsigned k=0;k<c->history_count;k++)if(c->history[k].descriptor.file_id==d->file_id||c->history[k].descriptor.ordinal==d->ordinal)return STORE_INVALID;
 StorageQueueHealth h;StorageService *s=c->services[slot];if(s->queue->file_id!=d->file_id||storage_queue_health(s->queue,&h)!=STORE_OK||!h.input_paused||h.live_jobs||h.retained_receipts||h.accepted)return STORE_BUSY;
 c->history[c->history_count++]=c->files[slot];memset(&c->files[slot],0,sizeof(c->files[slot]));c->files[slot].descriptor=*d;c->active=slot;c->replacing=true;encode(c,false);c->phase=CATALOG_OPEN_WRITE;return STORE_OK;
}
bool storage_file_catalog_resolve(const char *catalog,const StorageFileDescriptor *d,char *out,size_t capacity){
 if(!catalog||!d||!out||!capacity||!descriptor_valid(d))return false;
 const char *slash=strrchr(catalog,'/');if(!slash)return false;size_t parent=(size_t)(slash-catalog);if(parent>191)return false;
 int n=d->location==CATALOG_BASE?snprintf(out,capacity,"%.*s/%s",(int)parent,catalog,d->name):snprintf(out,capacity,"%.*s/EV/%08X/%08X/%s",(int)parent,catalog,(unsigned)(d->ordinal>>32),(unsigned)d->ordinal,d->name);
 return n>0&&(size_t)n<capacity;
}
