#include "storage_multi_sink.h"
#include <string.h>
static StorageResult available(StorageMultiSink *s){return s->fatal?STORE_INVALID:s->closing||s->active?STORE_BUSY:s->next_ticket==UINT64_MAX?STORE_EXHAUSTED:STORE_OK;}
static StorageResult preflight(StorageMultiSink *s,const StorageMultiSource *src){
 StorageResult r=available(s);if(r!=STORE_OK)return r;
 return s->observer.preflight?s->observer.preflight(s->observer.context,src):STORE_OK;
}
static StorageResult accepted(StorageMultiSink *s,StoragePump *p,StorageResult r,StoragePumpTicket local,StoragePumpTicket *out,const StorageMultiSource *src){
 if(r!=STORE_OK){if(s->observer.cancel)s->observer.cancel(s->observer.context);return r;}
 s->active_pump=p;s->underlying=local;s->public_ticket=(StoragePumpTicket){s->next_ticket++,0};s->active=true;*out=s->public_ticket;
 if(s->observer.commit&&!s->observer.commit(s->observer.context,src,p,local))s->fatal=true;
 return STORE_OK;
}
static StorageResult energy(void *ctx,const LfnServiceMessage *m,StoragePumpTicket *out){
 StorageMultiSink *s=ctx;if(!s||!m||!out)return STORE_INVALID;
 /* Bridge delivery-loss notices span identities and carry only delivery
  * ordinals. The service persists them under this sink's session without
  * inventing a capture/config identity or raw sample range. */
 if(m->identity.session_id!=s->session_id&&
    (m->kind!=SERVICE_DELIVERY_LOSS||m->identity.session_id))return STORE_INVALID;
 StorageMultiSource src={.kind=PUMP_ENERGY,.session=s->session_id,.value.energy=m};
 StorageResult r=preflight(s,&src);if(r!=STORE_OK)return r;StoragePumpTicket local={0};r=storage_pump_energy(s->targets.energy,m,&local);return accepted(s,s->targets.energy,r,local,out,&src);
}
static StorageResult psd(void *ctx,const LfnSpectrum *m,StoragePumpTicket *out){
 StorageMultiSink *s=ctx;if(!s||!m||!out||m->session_id!=s->session_id)return STORE_INVALID;
 StorageMultiSource src={.kind=PUMP_PSD,.session=s->session_id,.value.psd=m};
 StorageResult r=preflight(s,&src);if(r!=STORE_OK)return r;StoragePumpTicket local={0};r=storage_pump_psd(s->targets.psd,m,&local);return accepted(s,s->targets.psd,r,local,out,&src);
}
static StorageResult pcm(void *ctx,uint64_t session,const uint8_t *bytes,size_t n,StoragePumpTicket *out){
 StorageMultiSink *s=ctx;PacketView view;if(!s||!bytes||!out||session!=s->session_id||!packet_decode(bytes,n,&view)||(view.header.type!=LFN_PCM&&view.header.type!=LFN_RAW))return STORE_INVALID;
 StorageMultiSource src={.kind=PUMP_PCM,.session=session,.bytes=n,.value.pcm=bytes};
 StorageResult r=preflight(s,&src);if(r!=STORE_OK)return r;StoragePump *p=view.header.type==LFN_RAW?s->targets.raw:s->targets.pcm;StoragePumpTicket local={0};r=storage_pump_pcm(p,session,bytes,n,&local);return accepted(s,p,r,local,out,&src);
}
static StorageResult loss(void *ctx,const StorageIngressLoss *m,StoragePumpTicket *out){
 StorageMultiSink *s=ctx;if(!s||!m||!out||m->identity.session_id!=s->session_id)return STORE_INVALID;
 /* A route-overflow summary has no stream identity. Keep its session-level
  * notice with monitoring energy; the service validates its unknown range. */
 StoragePump *p=m->stream==0?s->targets.energy:m->stream==LFN_FFT?s->targets.psd:m->stream==LFN_PCM?s->targets.pcm:m->stream==LFN_RAW?s->targets.raw:NULL;if(!p)return STORE_INVALID;
 StorageMultiSource src={.kind=PUMP_INGRESS_LOSS,.session=s->session_id,.value.loss=m};
 StorageResult r=preflight(s,&src);if(r!=STORE_OK)return r;StoragePumpTicket local={0};r=storage_pump_ingress_loss(p,m,&local);return accepted(s,p,r,local,out,&src);
}
static bool exact(StorageMultiSink *s,StoragePumpTicket t){return s&&s->active&&t.slot==s->public_ticket.slot&&t.generation==s->public_ticket.generation;}
static StorageResult poll(void *ctx,StoragePumpTicket t,StoragePumpResult *out){StorageMultiSink *s=ctx;if(!out)return STORE_INVALID;return s&&s->fatal?STORE_INVALID:exact(s,t)?storage_pump_poll(s->active_pump,s->underlying,out):STORE_EXPIRED;}
static StorageResult ack(void *ctx,StoragePumpTicket t){
 StorageMultiSink *s=ctx;if(!exact(s,t))return STORE_EXPIRED;
 StorageResult r=storage_pump_ack(s->active_pump,s->underlying);if(r==STORE_OK){s->active=false;s->active_pump=NULL;}return r;
}
static StorageResult close_all(void *ctx){
 StorageMultiSink *s=ctx;if(!s||s->fatal)return STORE_INVALID;if(s->active)return STORE_BUSY;
 if(!s->closing&&s->observer.before_close){StorageResult r=s->observer.before_close(s->observer.context);if(r!=STORE_OK)return r;}
 s->closing=true;
 if(s->close_cursor<s->close_count){
  StorageResult r=storage_pump_close(s->close_pumps[s->close_cursor]);if(r!=STORE_OK)return r;
  s->close_cursor++;return s->close_cursor==s->close_count?STORE_OK:STORE_BUSY;
 }
 return STORE_OK;
}
bool storage_multi_sink_init(StorageMultiSink *s,uint64_t session,uint64_t seed,const StorageMultiTargets *targets,CaptureStorageSink *out){
 if(!s||!session||!seed||seed==UINT64_MAX||!targets||!out)return false;
 StoragePump *p[]={targets->energy,targets->psd,targets->pcm,targets->raw};
 for(unsigned k=0;k<4;k++)if(!p[k]||!p[k]->service||!p[k]->service->queue||p[k]->service->queue->session_id!=session)return false;
 StorageMultiTargets copy=*targets;memset(s,0,sizeof(*s));s->session_id=session;s->next_ticket=seed;s->targets=copy;
 for(unsigned k=0;k<4;k++){bool duplicate=false;for(unsigned j=0;j<s->close_count;j++)if(s->close_pumps[j]==p[k])duplicate=true;if(!duplicate)s->close_pumps[s->close_count++]=p[k];}
 *out=(CaptureStorageSink){s,energy,psd,pcm,loss,poll,ack,close_all};return true;
}

bool storage_multi_sink_set_close_pumps(StorageMultiSink *s,StoragePump *const *p,unsigned count){
 if(!s||!s->session_id||s->active||s->closing||!p||!count||count>STORAGE_FAMILY_FILES)return false;
 for(unsigned k=0;k<count;k++){
  if(!p[k]||!p[k]->service||!p[k]->service->queue||p[k]->service->queue->session_id!=s->session_id)return false;
  for(unsigned j=0;j<k;j++)if(p[j]==p[k])return false;
 }
 StoragePump *targets[]={s->targets.energy,s->targets.psd,s->targets.pcm,s->targets.raw};
 for(unsigned k=0;k<4;k++){bool found=false;for(unsigned j=0;j<count;j++)if(targets[k]==p[j])found=true;if(!found)return false;}
 memcpy(s->close_pumps,p,count*sizeof(*p));s->close_count=count;s->close_cursor=0;return true;
}

bool storage_multi_sink_matches(StorageMultiSink *s,const CaptureStorageSink *v){return s&&v&&v->context==s&&v->energy==energy&&v->psd==psd&&v->pcm==pcm&&v->loss==loss&&v->poll==poll&&v->ack==ack&&v->close==close_all;}

bool storage_multi_sink_view(StorageMultiSink *s,CaptureStorageSink *out){
 if(!s||!out||!s->session_id||!s->next_ticket||s->closing||s->active)return false;
 *out=(CaptureStorageSink){s,energy,psd,pcm,loss,poll,ack,close_all};return true;
}
