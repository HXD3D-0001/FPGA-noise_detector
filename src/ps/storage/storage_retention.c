#include "storage_retention.h"
#include <string.h>
static bool older(const StorageRetentionFile *a,const StorageRetentionFile *b){return a->session_id<b->session_id||(a->session_id==b->session_id&&(a->order_key<b->order_key||(a->order_key==b->order_key&&a->file_id<b->file_id)));}
StorageRetentionPolicy storage_retention_default(void){
 const uint64_t mib=UINT64_C(1024)*1024;StorageRetentionPolicy p={0};p.total_bytes=8192*mib;p.reserve_free_bytes=512*mib;
 p.limit[RETAIN_PSD]=2048*mib;p.limit[RETAIN_PCM]=2048*mib;p.limit[RETAIN_SUMMARY]=1024*mib;p.limit[RETAIN_NOTES]=256*mib;p.limit[RETAIN_METADATA]=64*mib;
 p.target_age_seconds[RETAIN_PSD]=7*86400;p.target_age_seconds[RETAIN_SUMMARY]=30*86400;return p;
}
bool storage_retention_begin(StorageRetentionScan *s,const StorageRetentionPolicy *p,StorageRetentionFamily family,uint64_t incoming,uint64_t free_bytes){
 if(!s||!p||(unsigned)family>=RETAIN_FAMILIES||!p->total_bytes)return false;
 for(unsigned i=0;i<RETAIN_FAMILIES;i++)if(!p->limit[i])return false;
 StorageRetentionPolicy copy=*p;memset(s,0,sizeof(*s));s->policy=copy;s->incoming_family=family;s->incoming_bytes=incoming;s->free_bytes=free_bytes;return true;
}
bool storage_retention_feed(StorageRetentionScan *s,const StorageRetentionFile *f){
 if(!s||!f||s->invalid)return false;
 if(!f->session_id||!f->file_id||!f->catalog_id||!f->ledger_id||(unsigned)f->family>=RETAIN_FAMILIES||f->bytes>UINT64_MAX-s->total||f->bytes>UINT64_MAX-s->by_family[f->family]||s->count==UINT64_MAX){s->invalid=true;return false;}
 s->total+=f->bytes;s->by_family[f->family]+=f->bytes;s->count++;
 if(!f->closed||!f->seal_verified||f->active||f->journal||f->references)return true;
 unsigned k=(unsigned)f->family;
 if(!s->have[k]||older(f,&s->oldest[k])){s->oldest[k]=*f;s->have[k]=true;}
 if(f->age_known&&s->policy.target_age_seconds[k]&&f->age_seconds>=s->policy.target_age_seconds[k]&&(!s->have_expired||older(f,&s->expired))){s->expired=*f;s->have_expired=true;}
 return true;
}
StorageRetentionAction storage_retention_finish(const StorageRetentionScan *s,StorageRetentionFile *out){
 if(!s||!out||s->invalid)return RETENTION_INVALID;
 unsigned k=(unsigned)s->incoming_family;
 if(s->incoming_bytes>s->policy.limit[k]||s->incoming_bytes>s->policy.total_bytes)return RETENTION_NO_SPACE;
 if(s->by_family[k]>s->policy.limit[k]-s->incoming_bytes){if(!s->have[k])return RETENTION_NO_SPACE;*out=s->oldest[k];return RETENTION_DELETE_ONE;}
 bool full=s->total>s->policy.total_bytes-s->incoming_bytes||s->free_bytes<s->policy.reserve_free_bytes||s->incoming_bytes>s->free_bytes-s->policy.reserve_free_bytes;
 if(!full)return RETENTION_KEEP;
 if(s->have_expired){*out=s->expired;return RETENTION_DELETE_ONE;}
 for(unsigned i=0;i<RETAIN_FAMILIES;i++)if(s->have[i]){*out=s->oldest[i];return RETENTION_DELETE_ONE;}
 return RETENTION_NO_SPACE;
}
