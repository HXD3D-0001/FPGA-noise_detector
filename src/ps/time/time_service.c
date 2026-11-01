#include "time_service.h"
#include <string.h>
#include <math.h>
#include <stdio.h>
static StorageSubmit reject(StorageResult r){StorageSubmit s={0};s.result=r;return s;}
bool lfn_time_service_init(LfnTimeService *t,StorageService *s,uint64_t session){
 if(!t||!s||!s->queue||session!=s->queue->session_id)return false;
 memset(t,0,sizeof(*t));t->storage=s;t->session_id=session;return true;
}
StorageSubmit lfn_time_service_submit(LfnTimeService *t,const StorageTimeFacts *f,double ppm){
 if(!t||!f||f->session_id!=t->session_id||!isfinite(ppm)||ppm<0||ppm>1000000||!isfinite(f->ns_per_raw)||f->ns_per_raw<=0)return reject(STORE_INVALID);
 if(t->pending)return reject(STORE_BUSY);
 double fs=1000000000.0/f->ns_per_raw;if(!isfinite(fs)||fs<1||fs>1000000)return reject(STORE_INVALID);
 StorageRequest r;if(!storage_build_time(&t->builder,f,&r,t->payload,sizeof(t->payload)))return reject(STORE_INVALID);
 if(!r.payload_bytes||r.payload[r.payload_bytes-1]!='}')return reject(STORE_INVALID);
 int extra=snprintf((char*)t->payload+r.payload_bytes-1,sizeof(t->payload)-r.payload_bytes+1,",\"fs_uncertainty_ppm\":%.17g}",ppm);
 if(extra<0||(size_t)extra>=sizeof(t->payload)-r.payload_bytes+1)return reject(STORE_INVALID);
 r.payload_bytes=r.payload_bytes-1+(size_t)extra;
 StorageSubmit result=storage_service_metadata(t->storage,&r.identity,r.payload,r.payload_bytes);
 if(result.result==STORE_OK){t->pending=true;t->pending_ticket=result.ticket;t->pending_identity=r.identity;t->pending_identity.record_seq=result.record_seq;t->pending_synchronized=f->synchronized;t->pending_map=(LfnTimeMapEntry){.map_id=f->map_id,.session_id=f->session_id,.capture_id=f->capture_id,.valid_from_raw=f->anchor_raw,.config_id=f->config_id,.utc_at_valid_from_ns=f->utc_ns,.raw_fs_hz=fs,.fs_uncertainty_ppm=ppm,.base_uncertainty_ns=f->uncertainty_ns};}
 return result;
}
void lfn_time_service_poll(LfnTimeService *t){
 if(!t||!t->pending)return;
 StorageReceipt r;StorageResult result=storage_queue_poll(t->storage->queue,t->pending_ticket,&r);if(result==STORE_BUSY)return;
 if(result!=STORE_OK||t->storage->health.fatal){t->pending=false;t->failed=true;return;}
 if(r.state!=STORE_SYNCED&&r.state!=STORE_FAILED)return;
 t->pending=false;
 if(r.state!=STORE_SYNCED||r.identity.type!=RECORD_TIME_MAP||r.identity.session_id!=t->pending_identity.session_id||r.identity.capture_id!=t->pending_identity.capture_id||r.identity.config_id!=t->pending_identity.config_id||r.identity.record_seq!=t->pending_identity.record_seq||r.identity.first_raw!=t->pending_identity.first_raw||r.identity.end_raw!=t->pending_identity.end_raw){t->failed=true;return;}
 memset(&t->active,0,sizeof(t->active));t->synchronized=t->pending_synchronized;t->failed=false;
 if(t->synchronized&&!lfn_time_map_append(&t->active,&t->pending_map)){t->synchronized=false;t->failed=true;}
}
bool lfn_time_service_tick(LfnTimeService *t,uint64_t now,bool requested,bool available){return t&&lfn_monotonic_coverage_tick(&t->coverage,now,requested,available);}
bool lfn_time_service_utc(const LfnTimeService *t,LfnSampleStamp stamp,LfnUtcEstimate *out){return t&&!t->failed&&t->synchronized&&!(t->pending&&stamp.session_id==t->pending_map.session_id&&stamp.capture_id==t->pending_map.capture_id&&stamp.raw_index>=t->pending_map.valid_from_raw)&&lfn_time_to_utc(&t->active,stamp,out);}
bool lfn_time_record_map(const RecordIdentity *id,const uint8_t *b,size_t n,MetadataJson *d,LfnTimeMapEntry *out,bool *synchronized){
 StorageMetadata m;if(!out||!synchronized||!id||id->type!=RECORD_TIME_MAP||!storage_metadata_decode(id,b,n,d,&m))return false;
 LfnTimeMapEntry e={0};int slope=metadata_json_field(d,0,"slope_ns_per_raw");double ns=0;
 if(!metadata_json_double(d,slope,&ns)||ns<=0)return false;
 e.map_id=m.object_id;e.session_id=id->session_id;e.capture_id=id->capture_id;e.config_id=id->config_id;e.valid_from_raw=m.origin_raw;e.raw_fs_hz=1000000000.0/ns;e.base_uncertainty_ns=m.uncertainty;
 if(!isfinite(e.raw_fs_hz)||e.raw_fs_hz<1||e.raw_fs_hz>1000000)return false;
 if(m.synchronized&&!metadata_json_i64(d,metadata_json_field(d,0,"utc_ns"),&e.utc_at_valid_from_ns))return false;
 if(m.synchronized&&(!metadata_json_double(d,metadata_json_field(d,0,"fs_uncertainty_ppm"),&e.fs_uncertainty_ppm)||e.fs_uncertainty_ppm<0||e.fs_uncertainty_ppm>1000000))return false;
 /* Legacy maps without a declared drift bound remain inspectable as metadata,
  * but cannot silently become zero-drift historical UTC estimates. */
 *out=e;*synchronized=m.synchronized;return true;
}
