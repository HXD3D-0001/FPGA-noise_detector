#include "raw_capture.h"
#include <string.h>
static bool lock(LfnRawCapture*r){return !r->locks.lock||r->locks.lock(r->locks.context);}
static void unlock(LfnRawCapture*r){if(r->locks.unlock)r->locks.unlock(r->locks.context);}
bool lfn_raw_init(LfnRawCapture*r,uint8_t*b,size_t n,const LfnRawLocks*l){
 if(!r||!b||n<LFN_RAW_PACKET_BYTES||!l||!!l->lock!=!!l->unlock)return false;
 memset(r,0,sizeof(*r));r->buffer=b;r->buffer_bytes=n;r->locks=*l;return true;
}
bool lfn_raw_policy_valid(const LfnRawCapture*r,const LfnRawPolicy*p){
 return r&&r->buffer&&p&&p->duration_us&&p->capacity_bytes>=LFN_RAW_PACKET_BYTES&&p->capacity_bytes<=r->buffer_bytes;
}
bool lfn_raw_begin(LfnRawCapture*r,uint64_t session,uint64_t capture,uint32_t config,const LfnRawPolicy*p){
 if(!lfn_raw_policy_valid(r,p)||!session||!capture||!config||
    (r->status.prepared&&r->status.session_id==session&&capture<=r->status.capture_id)||!lock(r))return false;
 memset(&r->status,0,sizeof(r->status));r->status.session_id=session;r->status.capture_id=capture;r->status.config_id=config;
 r->status.policy=*p;r->status.prepared=true;r->read_session=session;r->read_capture=capture;r->read_config=config;r->committed=0;
 unlock(r);return true;
}
void lfn_raw_start(LfnRawCapture*r,uint64_t now){
 if(r&&r->status.prepared&&!r->status.started&&!r->status.terminal&&r->status.reason==RAW_STOP_NONE){r->status.started=true;r->status.started_us=now;r->last_us=now;}
}
void lfn_raw_stop(LfnRawCapture*r,LfnRawStop reason){
 if(r&&r->status.prepared&&!r->status.terminal&&r->status.reason==RAW_STOP_NONE&&reason!=RAW_STOP_NONE)r->status.reason=reason;
}
void lfn_raw_tick(LfnRawCapture*r,uint64_t now){
 if(!r||!r->status.started||r->status.terminal)return;
 if(now<r->last_us)lfn_raw_stop(r,RAW_STOP_CLOCK);
 else if(now-r->status.started_us>=r->status.policy.duration_us)lfn_raw_stop(r,RAW_STOP_TIME);
 r->last_us=now;
}
void lfn_raw_finish(LfnRawCapture*r,bool failed){
 if(!r||!r->status.prepared||r->status.terminal)return;
 if(r->status.reason==RAW_STOP_NONE)r->status.reason=failed?RAW_STOP_FAULT:RAW_STOP_MANUAL;
 r->status.terminal=true;r->status.cleanup_failed=failed;
}
LfnRawResult lfn_raw_offer(LfnRawCapture*r,const uint8_t*b,size_t n,uint64_t now){
 if(!r||!r->status.started||r->status.terminal)return RAW_INVALID;
 lfn_raw_tick(r,now);
 if(r->status.reason!=RAW_STOP_NONE){if(r->status.ignored_after_stop!=UINT64_MAX)r->status.ignored_after_stop++;return RAW_EMPTY;}
 if(!lock(r)){lfn_raw_stop(r,RAW_STOP_LOCK_BUSY);return RAW_BUSY;}
 PacketView p;
 bool valid=b&&n==LFN_RAW_PACKET_BYTES&&packet_decode(b,n,&p)&&p.header.type==LFN_RAW&&
 p.header.capture_id==r->status.capture_id&&p.header.config_id==r->status.config_id&&
 (p.header.flags&LFN_VALID)&&!(p.header.flags&(LFN_WARMUP|LFN_GAP|LFN_PROTOCOL_ERROR|LFN_CLOCK_ERROR));
 if(!valid){unlock(r);lfn_raw_stop(r,RAW_STOP_INPUT_LOSS);return RAW_INVALID;}
 if(n>r->status.policy.capacity_bytes-r->committed){unlock(r);lfn_raw_stop(r,RAW_STOP_CAPACITY);return RAW_EMPTY;}
 memcpy(r->buffer+r->committed,b,n);r->committed+=n;r->status.stored_bytes=r->committed;r->status.packets++;
 bool full=r->status.policy.capacity_bytes-r->committed<LFN_RAW_PACKET_BYTES;
 unlock(r);if(full)lfn_raw_stop(r,RAW_STOP_CAPACITY);return RAW_OK;
}
LfnRawResult lfn_raw_read(LfnRawCapture*r,uint64_t session,uint64_t capture,uint32_t config,uint64_t ordinal,uint8_t*out,size_t n){
 if(!r||!r->buffer||!out||n<LFN_RAW_PACKET_BYTES)return RAW_INVALID;
 if(!lock(r))return RAW_BUSY;
 LfnRawResult result=RAW_OK;
 if(session!=r->read_session||capture!=r->read_capture||config!=r->read_config||!session||!capture||!config)result=RAW_STALE;
 else if(ordinal>=r->committed/LFN_RAW_PACKET_BYTES)result=RAW_EMPTY;
 else memcpy(out,r->buffer+(size_t)ordinal*LFN_RAW_PACKET_BYTES,LFN_RAW_PACKET_BYTES);
 unlock(r);return result;
}
