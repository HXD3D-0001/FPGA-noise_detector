#include "history.h"
#include <string.h>
static bool take(atomic_flag *l){return !atomic_flag_test_and_set_explicit(l,memory_order_acquire);}
static void drop(atomic_flag *l){atomic_flag_clear_explicit(l,memory_order_release);}
static bool same(HistoryIdentity a,HistoryIdentity b){return a.session_id==b.session_id&&a.capture_id==b.capture_id&&a.config_id==b.config_id;}
static bool bit(const uint8_t *b,size_t n){return (b[n/8]&(1u<<(n%8)))!=0;}
static void lost(HistoryWatermark *w,size_t n){w->lost_samples=w->lost_samples>UINT64_MAX-n?UINT64_MAX:w->lost_samples+n;}
void history_init(PcmHistory *h){memset(h,0,sizeof(*h));atomic_flag_clear(&h->lock);for(unsigned k=0;k<HISTORY_BLOCKS;k++)atomic_flag_clear(&h->blocks[k].lock);}
bool history_begin(PcmHistory *h,HistoryIdentity id,bool monitor){
 if(!h||!id.session_id||!id.capture_id||!id.config_id||!take(&h->lock))return false;
 if(h->owner.generation==UINT64_MAX){h->published.active=false;drop(&h->lock);return false;}
 uint64_t generation=h->owner.generation+1;memset(&h->owner,0,sizeof(h->owner));h->owner.identity=id;h->owner.generation=generation;h->owner.active=monitor;h->published=h->owner;drop(&h->lock);return true;
}
HistoryStatus history_append(PcmHistory *h,HistoryIdentity id,uint64_t first,const int32_t *samples,const uint8_t *valid,size_t count){
 if(!h||!samples||!count||count>256||first>UINT64_MAX-count*48)return HISTORY_INVALID;
 HistoryWatermark *w=&h->owner;
 if(!w->active||!same(id,w->identity)||(w->have_samples&&(first<w->end_raw||(first-w->base_raw)%48)))return HISTORY_INVALID;
 for(size_t k=0;k<count;k++)if((!valid||bit(valid,k))&&(samples[k]<-8388608||samples[k]>8388607))return HISTORY_INVALID;
 if(!w->have_samples){w->base_raw=first;w->have_samples=true;}
 uint64_t start=(first-w->base_raw)/48;HistoryStatus result=HISTORY_OK;
 for(size_t done=0;done<count;){
  uint64_t index=(start+done)/256;size_t offset=(size_t)((start+done)%256),n=256-offset;if(n>count-done)n=count-done;
  HistoryBlock *b=&h->blocks[index%HISTORY_BLOCKS];
  if(take(&b->lock)){
   if(b->generation!=w->generation||b->block_index!=index){
    if(b->generation!=w->generation||b->block_index+HISTORY_BLOCKS!=index)memset(b->valid,0,sizeof(b->valid));
    b->identity=id;b->generation=w->generation;b->block_index=index;b->first_raw=w->base_raw+index*256*48;b->end_raw=b->first_raw;b->split=0;
   }
   for(size_t j=b->split;j<offset;j++)b->valid[j/8]&=(uint8_t)~(1u<<(j%8));
   b->split=(unsigned)(offset+n);
   for(size_t j=0;j<n;j++){size_t at=offset+j;b->samples[at]=samples[done+j];if(!valid||bit(valid,done+j))b->valid[at/8]|=(uint8_t)(1u<<(at%8));else b->valid[at/8]&=(uint8_t)~(1u<<(at%8));}
   b->end_raw=first+(done+n)*48;drop(&b->lock);
  }else{lost(w,n);result=HISTORY_BUSY;}
  done+=n;
 }
 w->end_raw=first+count*48;w->first_raw=w->end_raw-w->base_raw>UINT64_C(60000)*48?w->end_raw-UINT64_C(60000)*48:w->base_raw;
 if(take(&h->lock)){h->published=*w;drop(&h->lock);}else result=HISTORY_BUSY;
 return result;
}
HistoryStatus history_append_packet(PcmHistory *h,uint64_t session,const PacketView *p){
 if(!p||p->header.type!=LFN_PCM||p->value_count>256||!(p->header.flags&LFN_VALID))return HISTORY_INVALID;
 int32_t samples[256];for(size_t k=0;k<p->value_count;k++)if(!packet_value(p,k,&samples[k]))return HISTORY_INVALID;
 HistoryIdentity id={session,p->header.capture_id,p->header.config_id};return history_append(h,id,p->header.first_raw_sample_index,samples,NULL,p->value_count);
}
bool history_watermark(PcmHistory *h,HistoryWatermark *w){if(!h||!w||!take(&h->lock))return false;*w=h->published;drop(&h->lock);return true;}
bool history_copy(PcmHistory *h,const HistoryWatermark *frozen,uint64_t first,size_t count,int32_t *samples,uint8_t *valid,HistoryReadResult *r){
 if(!h||!frozen||!samples||!valid||!r||!count||count>4096||first>UINT64_MAX-count*48||!frozen->active||!frozen->have_samples||first<frozen->base_raw||(first-frozen->base_raw)%48||first+count*48>frozen->end_raw)return false;
 *r=(HistoryReadResult){first,first+count*48,count,0,0};memset(valid,0,(count+7)/8);
 HistoryWatermark now;if(!history_watermark(h,&now)){r->reasons=HISTORY_CONFLICT;return false;}
 if(!now.active||now.generation!=frozen->generation||!same(now.identity,frozen->identity)){r->reasons=HISTORY_IDENTITY;return false;}
 uint64_t start=(first-frozen->base_raw)/48;
 for(size_t done=0;done<count;){
  uint64_t index=(start+done)/256;size_t offset=(size_t)((start+done)%256),n=256-offset;if(n>count-done)n=count-done;
  HistoryBlock *b=&h->blocks[index%HISTORY_BLOCKS];bool held=false;
  for(unsigned attempt=0;attempt<3&&!held;attempt++)held=take(&b->lock);
  if(!held)r->reasons|=HISTORY_CONFLICT;
  else{
   bool match=b->generation==frozen->generation&&same(b->identity,frozen->identity);
   for(size_t k=0;k<n;k++){
    uint64_t raw=first+(done+k)*48;
    if(raw<now.first_raw||raw<frozen->first_raw)r->reasons|=HISTORY_EVICTED;
    else if(match&&((b->block_index==index&&offset+k<b->split)||(b->block_index==index+HISTORY_BLOCKS&&offset+k>=b->split))&&bit(b->valid,offset+k)){samples[done+k]=b->samples[offset+k];valid[(done+k)/8]|=(uint8_t)(1u<<((done+k)%8));r->valid_count++;}
    else r->reasons|=HISTORY_GAP;
   }
   drop(&b->lock);
  }
  done+=n;
 }
 if(!history_watermark(h,&now)){memset(valid,0,(count+7)/8);r->valid_count=0;r->reasons|=HISTORY_CONFLICT;return false;}
 if(!now.active||now.generation!=frozen->generation){memset(valid,0,(count+7)/8);r->valid_count=0;r->reasons|=HISTORY_IDENTITY;return false;}
 return true;
}
