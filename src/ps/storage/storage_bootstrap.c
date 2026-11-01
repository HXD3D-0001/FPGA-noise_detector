#include "storage_bootstrap.h"
#include <string.h>
static int find_entry(const StorageMetadataRegistry *r,RecordType type,uint64_t session,uint32_t config,uint64_t capture,uint64_t object,uint64_t before,uint64_t revision){
 int best=-1;unsigned count=atomic_load(&r->published);for(unsigned k=0;k<count;k++){const StorageMetadata *m=&r->entries[k].value;const RecordIdentity *id=&m->identity;if(id->type!=type||id->session_id!=session||id->record_seq>=before||(config&&id->config_id!=config)||(capture&&id->capture_id!=capture)||(object&&m->object_id!=object)||(revision&&m->revision!=revision))continue;if(best<0||id->record_seq>r->entries[best].value.identity.record_seq)best=(int)k;}return best;
}
typedef struct {StorageBootstrapPlan *plan;StorageMetadataRegistry *registry;const StorageFamily *family;uint64_t file;uint8_t visited[STORAGE_METADATA_ENTRIES];} Closure;
static bool visit(Closure *c,int k,unsigned depth){
 if(k<0||depth>STORAGE_METADATA_ENTRIES)return false;
 if(c->visited[k]==2)return true;
 if(c->visited[k]==1)return false;
 c->visited[k]=1;
 const StorageMetadataEntry *e=&c->registry->entries[k];const StorageMetadata *m=&e->value;const RecordIdentity *id=&m->identity;StorageFamilyEntry proof;
 if(storage_family_query(c->family,id->record_seq,&proof)!=STORE_OK||proof.file_id!=c->file||proof.state!=FAMILY_SAVED||!proof.terminal_durable)return false;
 if(id->type!=RECORD_SESSION&&!visit(c,find_entry(c->registry,RECORD_SESSION,id->session_id,0,0,0,id->record_seq,0),depth+1))return false;
 if(id->type!=RECORD_SESSION&&id->type!=RECORD_CONFIG&&!visit(c,find_entry(c->registry,RECORD_CONFIG,id->session_id,id->config_id,0,0,id->record_seq,0),depth+1))return false;
 if(id->type==RECORD_CAPTURE&&m->revision>1&&!visit(c,find_entry(c->registry,RECORD_CAPTURE,id->session_id,id->config_id,id->capture_id,0,id->record_seq,m->revision-1),depth+1))return false;
 if(id->type==RECORD_TIME_MAP&&!visit(c,find_entry(c->registry,RECORD_CAPTURE,id->session_id,id->config_id,id->capture_id,0,id->record_seq,0),depth+1))return false;
 if(id->type==RECORD_CALIBRATION&&m->calibrated&&!visit(c,find_entry(c->registry,RECORD_TIME_MAP,id->session_id,0,0,m->object_id,id->record_seq,0),depth+1))return false;
 if(c->plan->count==STORAGE_BOOTSTRAP_DEFINITIONS||e->bytes>STORAGE_METADATA_BYTES)return false;
 StorageDefinitionCopy *out=&c->plan->records[c->plan->count++];out->identity=*id;out->identity.record_seq=0;out->bytes=e->bytes;memcpy(out->payload,e->payload,e->bytes);c->visited[k]=2;return true;
}
StorageResult storage_bootstrap_plan(StorageBootstrapPlan *p,StorageService *s,const StorageFamily *f,uint64_t file,uint32_t config,uint64_t capture){
 if(!p||!s||!s->registry||!s->health.sealed||!s->health.drained||!f||f->phase!=FAMILY_READY||!config||!capture||s->queue->file_id!=file||f->session_id!=s->queue->session_id)return STORE_INVALID;
 memset(p,0,sizeof(*p));p->session_id=f->session_id;Closure c={.plan=p,.registry=s->registry,.family=f,.file=file};
 int cfg=find_entry(s->registry,RECORD_CONFIG,p->session_id,config,0,0,UINT64_MAX,0),cap=find_entry(s->registry,RECORD_CAPTURE,p->session_id,config,capture,0,UINT64_MAX,0);if(cfg<0||cap<0)return STORE_DEPENDENCY;
 if(!visit(&c,cfg,0))return STORE_DEPENDENCY;
 uint64_t calibration=s->registry->entries[cfg].value.calibration_id;if(calibration&&!visit(&c,find_entry(s->registry,RECORD_CALIBRATION,p->session_id,config,0,calibration,UINT64_MAX,0),0))return STORE_DEPENDENCY;
 if(!visit(&c,cap,0))return STORE_DEPENDENCY;
 int map=find_entry(s->registry,RECORD_TIME_MAP,p->session_id,config,capture,0,UINT64_MAX,0);if(map>=0&&!visit(&c,map,0))return STORE_DEPENDENCY;
 p->valid=true;return STORE_OK;
}
bool storage_bootstrap_init(StorageBootstrapWriter *b,const StorageBootstrapPlan *p,StorageService *s){
 StorageQueueHealth h;if(!b||!p||!p->valid||!p->count||!s||!s->queue||!s->registry||s->queue->session_id!=p->session_id||s->health.sealed||s->health.fatal||atomic_load(&s->registry->published)||storage_queue_health(s->queue,&h)!=STORE_OK||h.accepted||h.retained_receipts||h.live_jobs)return false;
 memset(b,0,sizeof(*b));b->plan=p;b->service=s;b->state=BOOTSTRAP_SUBMIT;return true;
}
void storage_bootstrap_step(StorageBootstrapWriter *b){
 if(!b)return;
 if(b->service->health.fatal){b->state=BOOTSTRAP_FAILED;return;}
 if(b->state==BOOTSTRAP_SUBMIT){const StorageDefinitionCopy *d=&b->plan->records[b->cursor];StorageSubmit result=storage_service_metadata_reserved(b->service,&d->identity,d->payload,d->bytes,true);if(result.result==STORE_BUSY)return;
 if(result.result==STORE_CAPACITY)return; /* family admission backlog: retry after fair drain */
 if(result.result!=STORE_OK){b->state=BOOTSTRAP_FAILED;return;}b->ticket=result.ticket;b->state=BOOTSTRAP_WAIT;return;}
 if(b->state==BOOTSTRAP_WAIT){StorageReceipt r;StorageResult result=storage_queue_poll(b->service->queue,b->ticket,&r);if(result==STORE_BUSY)return;
 if(result!=STORE_OK||r.state==STORE_FAILED){b->state=BOOTSTRAP_FAILED;return;}if(r.state!=STORE_SYNCED)return;
 if(++b->cursor==b->plan->count)b->state=BOOTSTRAP_DONE;else b->state=BOOTSTRAP_SUBMIT;}
}
