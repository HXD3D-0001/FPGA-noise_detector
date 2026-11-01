#include "storage_archive_verify.h"
#include <string.h>
static bool definition(RecordType t){return t==RECORD_SESSION||t==RECORD_CONFIG||t==RECORD_CAPTURE||t==RECORD_TIME_MAP||t==RECORD_CALIBRATION;}
bool storage_archive_verify_init(StorageArchiveVerifier *v,StorageHistoryReader *r,const StorageBackend *data,const StorageBackend *ledger,uint64_t session,uint64_t ledger_id,const StorageCatalogEntry *e){
 if(!v||!r||!data||!ledger||!ledger->size||!ledger->read||!session||!ledger_id||!e||e->state!=CATALOG_SEALED||!e->descriptor.file_id||e->descriptor.file_id==ledger_id||e->family_prefix%512||!e->admission_cutoff||e->definitions>64||e->definitions>e->records||(!e->records&&(e->file_bytes||e->first_seq||e->last_seq||e->definitions||e->file_crc))||(e->records&&(!e->first_seq||e->first_seq>e->last_seq||e->last_seq>=e->admission_cutoff)))return false;
 StorageBackend copy=*ledger;StorageCatalogEntry expected=*e;if(!storage_history_init(r,data,session,e->descriptor.file_id,e->file_bytes,true,true,e->file_crc))return false;
 memset(v,0,sizeof(*v));v->reader=r;v->ledger=copy;v->recorded=expected;v->session_id=session;v->ledger_id=ledger_id;v->state=ARCHIVE_READ_DATA;return true;
}
void storage_archive_verify_step(StorageArchiveVerifier *v){
 if(!v)return;
 StorageHistoryReader *r=v->reader;
 if(v->state==ARCHIVE_READ_DATA){
  if(r->state==HISTORY_SIZE||r->state==HISTORY_HEADER||r->state==HISTORY_BODY){storage_history_step(r);return;}
  if(r->state==HISTORY_RECORD){
   bool ok=v->index_started?storage_family_index_next(&v->index,r->view.identity.record_seq):storage_family_index_init_observed(&v->index,&v->ledger,v->session_id,v->ledger_id,v->recorded.family_prefix,r->view.identity.record_seq);
   if(!ok){v->state=ARCHIVE_BAD;return;}v->index_started=true;v->state=ARCHIVE_ASSOCIATION;return;
  }
  if(r->state!=HISTORY_DONE||r->records!=v->recorded.records||v->definition_count!=v->recorded.definitions||v->first_sequence!=v->recorded.first_seq||v->last_sequence!=v->recorded.last_seq){v->state=ARCHIVE_BAD;return;}
  if(!v->index_started&&!storage_family_index_init_observed(&v->index,&v->ledger,v->session_id,v->ledger_id,v->recorded.family_prefix,1)){v->state=ARCHIVE_BAD;return;}
  if(!storage_family_index_drain(&v->index)){v->state=ARCHIVE_BAD;return;}v->index_started=true;v->state=ARCHIVE_DRAIN_LEDGER;return;
 }
 if(v->state==ARCHIVE_ASSOCIATION){
  storage_family_index_step(&v->index);StorageFamilyEntry e;StorageResult result=storage_family_index_result(&v->index,&e);if(result==STORE_BUSY)return;
  if(result!=STORE_OK||storage_family_index_check_record(&v->index,v->recorded.descriptor.file_id,r->offset,r->record,r->total,storage_registry_validate,&r->registry)!=STORE_OK){v->state=ARCHIVE_BAD;return;}
  for(unsigned k=0;k<e.dependency_count;k++){bool found=false;for(unsigned j=0;j<v->definition_count;j++)if(v->definitions[j]==e.dependencies[k])found=true;if(!found){v->state=ARCHIVE_BAD;return;}}
  if(definition(r->view.identity.type)){if(v->definition_count==64){v->state=ARCHIVE_BAD;return;}v->definitions[v->definition_count++]=r->view.identity.record_seq;}
  if(!v->first_sequence)v->first_sequence=r->view.identity.record_seq;
  v->last_sequence=r->view.identity.record_seq;(void)storage_history_advance(r);v->state=ARCHIVE_READ_DATA;return;
 }
 if(v->state==ARCHIVE_DRAIN_LEDGER){
  storage_family_index_step(&v->index);if(v->index.state==FAMILY_INDEX_SIZE||v->index.state==FAMILY_INDEX_RESERVE||v->index.state==FAMILY_INDEX_TERMINAL)return;
  uint64_t end=0;for(unsigned k=0;k<v->index.file_count;k++)if(v->index.files[k]==v->recorded.descriptor.file_id)end=v->index.ends[k];
  v->state=v->index.state==FAMILY_INDEX_NOT_FOUND&&v->index.last_seq<v->recorded.admission_cutoff&&end==v->recorded.file_bytes?ARCHIVE_READ_REVALIDATED:ARCHIVE_BAD;
 }
}
void storage_archive_verify_cancel(StorageArchiveVerifier *v){if(v&&(v->state==ARCHIVE_READ_DATA||v->state==ARCHIVE_ASSOCIATION||v->state==ARCHIVE_DRAIN_LEDGER)){storage_history_cancel(v->reader);storage_family_index_cancel(&v->index);v->state=ARCHIVE_CANCELLED;}}
