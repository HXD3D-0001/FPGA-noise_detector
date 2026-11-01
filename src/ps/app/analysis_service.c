#include "analysis_service.h"
#include "../time/time_map.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void increment(uint64_t *n) { if(*n!=UINT64_MAX)++*n; }
static bool pool_lock(LfnAnalysisService *s) {
    return !s->hooks.pool_lock || s->hooks.pool_lock(s->hooks.context);
}
static void pool_unlock(LfnAnalysisService *s) {
    if(s->hooks.pool_unlock)s->hooks.pool_unlock(s->hooks.context);
}
static void snapshot_lock(LfnAnalysisService *s) {
    if(s->hooks.snapshot_lock)s->hooks.snapshot_lock(s->hooks.context);
}
static void snapshot_unlock(LfnAnalysisService *s) {
    if(s->hooks.snapshot_unlock)s->hooks.snapshot_unlock(s->hooks.context);
}
bool lfn_analysis_init(LfnAnalysisService *s,BlockPool *p,uint64_t session,uint64_t capture,
                        const LfnAnalysisConfig *cfg,const LfnAnalysisHooks *h) {
    if(!s || !p || !session || !capture || !cfg || !cfg->config_id || !h || !h->monotonic_ns ||
       (!!h->pool_lock != !!h->pool_unlock) || (!!h->snapshot_lock != !!h->snapshot_unlock))return false;
    memset(s,0,sizeof(*s));s->pool=p;s->session_id=session;s->capture_id=capture;s->config=*cfg;s->hooks=*h;
    for(unsigned i=0;i<2;++i) {
        s->snapshots[i].config=*cfg;
        s->snapshots[i].spectrum.session_id=session;s->snapshots[i].spectrum.capture_id=capture;
        s->snapshots[i].spectrum.config_id=cfg->config_id;
    }
    return true;
}
static void publish(LfnAnalysisService *s,bool accepted,uint64_t now) {
    snapshot_lock(s);
    unsigned next=s->active^1;
    s->snapshots[next]=s->snapshots[s->active];
    LfnAnalysisSnapshot *snapshot=&s->snapshots[next];
    if(snapshot->version!=UINT64_MAX)++snapshot->version;
    snapshot->config=s->config;snapshot->health=s->health;snapshot->health_at_ns=now;
    if(accepted) { snapshot->has_spectrum=true;snapshot->spectrum=s->working;snapshot->spectrum_at_ns=now; }
    s->active=next;snapshot_unlock(s);
}
static bool finish_release(LfnAnalysisService *s) {
    if(!s->pending_release)return true;
    if(!pool_lock(s)){increment(&s->health.pool_busy);return false;}
    bool released=block_pool_release(s->pool,s->release_handle);pool_unlock(s);
    if(!released){increment(&s->health.release_errors);return false;}
    s->pending_release=false;return true;
}
bool lfn_analysis_step(LfnAnalysisService *s) {
    if(!s || !s->pool || !s->hooks.monotonic_ns)return false;
    uint64_t started=s->hooks.monotonic_ns(s->hooks.context);
    if(!finish_release(s))return false;
    if(!pool_lock(s)){increment(&s->health.pool_busy);return false;}
    const uint8_t *bytes=NULL;size_t length=0;BlockHandle handle;
    bool have=block_pool_take(s->pool,&handle,&bytes,&length);
    if(s->pool->highwater>s->health.queue_highwater)s->health.queue_highwater=s->pool->highwater;
    pool_unlock(s);
    if(!have)return false;
    s->pending_release=true;s->release_handle=handle;
    PacketView p;bool accepted=false;
    if(!packet_decode(bytes,length,&p))increment(&s->health.rejected_packet);
    else if(p.header.capture_id!=s->capture_id || p.header.config_id!=s->config.config_id)
        increment(&s->health.rejected_identity);
    else if(p.header.type==LFN_RAW)increment(&s->health.ignored_raw);
    else if(p.header.type!=LFN_FFT)increment(&s->health.rejected_packet);
    else if(s->have_seq && p.header.stream_seq<=s->last_seq)increment(&s->health.rejected_order);
    else {
        uint64_t core_started=s->hooks.monotonic_ns(s->hooks.context);
        accepted=lfn_spectrum_from_packet(s->session_id,&p,&s->config,&s->working);
        uint64_t core_ended=s->hooks.monotonic_ns(s->hooks.context);
        if(core_ended>=core_started && core_ended-core_started>s->health.max_core_ns)
            s->health.max_core_ns=core_ended-core_started;
        if(accepted) {
            if(s->have_seq && p.header.stream_seq-s->last_seq>1) {
                uint64_t missing=p.header.stream_seq-s->last_seq-1;
                s->health.missing_frames=missing>UINT64_MAX-s->health.missing_frames?UINT64_MAX:s->health.missing_frames+missing;
            }
            s->have_seq=true;s->last_seq=p.header.stream_seq;increment(&s->health.accepted);
        } else increment(&s->health.rejected_packet);
    }
    (void)finish_release(s);
    uint64_t ended=s->hooks.monotonic_ns(s->hooks.context);
    if(ended>=started && ended-started>s->health.max_step_ns)s->health.max_step_ns=ended-started;
    publish(s,accepted,ended);return true;
}
bool lfn_analysis_snapshot_copy(LfnAnalysisService *s,LfnAnalysisSnapshot *destination) {
    if(!s || !destination)return false;
    snapshot_lock(s);
    bool available=s->snapshots[s->active].version!=0;
    if(available)*destination=s->snapshots[s->active];
    snapshot_unlock(s);return available;
}
typedef struct { char *bytes;size_t capacity,length;bool ok; } Writer;
static void append(Writer *w,const char *format,...) {
    if(!w->ok)return;
    va_list ap;va_start(ap,format);
    int result=vsnprintf(w->bytes+w->length,w->capacity-w->length,format,ap);va_end(ap);
    if(result<0 || (size_t)result>=w->capacity-w->length){w->ok=false;return;}
    w->length+=(size_t)result;
}
static void u64(Writer *w,uint64_t value) {
    char decimal[21];(void)lfn_u64_decimal(value,decimal,sizeof(decimal));append(w,"\"%s\"",decimal);
}
static void level(Writer *w,LfnLevel l) {
    if(l.status==LFN_LEVEL_VALID && isfinite(l.dbfs))append(w,"%.12g",l.dbfs);
    else append(w,"null");
}
bool lfn_analysis_snapshot_json(const LfnAnalysisSnapshot *s,uint64_t now,char *out,size_t capacity,size_t *written) {
    if(written)*written=0;
    if(out && capacity)out[0]=0;
    if(!s || !out || !capacity || !written || !s->version || now<s->health_at_ns ||
       (s->has_spectrum && now<s->spectrum_at_ns) || !isfinite(s->config.nominal_fs_hz) ||
       (s->config.measured_fs_valid && !isfinite(s->config.measured_fs_hz)))return false;
    const LfnSpectrum *p=&s->spectrum;
    if(s->has_spectrum) {
        if(!isfinite(p->target_power) || !isfinite(p->full_range_power) || !isfinite(p->fs_used_hz) ||
           !isfinite(p->window_sum_squares) || !isfinite(p->bin_spacing_hz) ||
           !isfinite(p->observation_seconds) || !isfinite(p->refresh_seconds))return false;
        for(size_t k=0;k<LFN_SPECTRUM_BINS;++k)if(!isfinite(p->psd[k]) || p->psd[k]<0)return false;
        if(p->peak_count>LFN_SPECTRUM_MAX_PEAKS)return false;
        for(size_t i=0;i<p->peak_count;++i)if(!isfinite(p->peaks[i].frequency_hz) || !isfinite(p->peaks[i].main_lobe_power))return false;
        for(size_t i=0;i<LFN_SPECTRUM_BANDS;++i) {
            const LfnSpectrumBand *b=&p->bands[i];
            if(!isfinite(b->nominal_center_hz) || !isfinite(b->exact_center_hz) ||
               !isfinite(b->low_hz) || !isfinite(b->high_hz) || !isfinite(b->power))return false;
        }
    }
    Writer w={out,capacity,0,true};
    append(&w,"{\"schema_version\":1,\"session_id\":");u64(&w,p->session_id);
    append(&w,",\"capture_id\":");u64(&w,p->capture_id);
    append(&w,",\"config_id\":%u,\"version\":",p->config_id);u64(&w,s->version);
    append(&w,",\"stream_seq\":");u64(&w,p->stream_seq);
    append(&w,",\"first_raw\":");u64(&w,p->first_raw);append(&w,",\"end_raw\":");u64(&w,p->end_raw);
    append(&w,",\"has_spectrum\":%s,\"valid\":%s,\"flags\":%u,\"blk_exp\":%u,\"test_source\":%s,\"calibrated\":false,\"unit\":\"full_scale_power_per_hz\",\"age_ns\":",
           s->has_spectrum?"true":"false",s->has_spectrum && p->valid?"true":"false",p->flags,p->blk_exp,s->config.test_source?"true":"false");
    if(s->has_spectrum)u64(&w,now-s->spectrum_at_ns);else append(&w,"null");
    append(&w,",\"nominal_fs_hz\":%.12g,\"measured_fs_hz\":",s->config.nominal_fs_hz);
    if(s->config.measured_fs_valid)append(&w,"%.12g",s->config.measured_fs_hz);else append(&w,"null");
    append(&w,",\"bin_spacing_hz\":%.12g,\"observation_seconds\":%.12g,\"refresh_seconds\":%.12g,\"window_sum_squares\":%.17g,\"target_power\":%.12g,\"target_dbfs\":",
           p->bin_spacing_hz,p->observation_seconds,p->refresh_seconds,p->window_sum_squares,p->target_power);
    level(&w,p->target_level);append(&w,",\"target_level_status\":%u,\"psd\":[",(unsigned)p->target_level.status);
    if(s->has_spectrum)for(size_t k=0;k<LFN_SPECTRUM_BINS;++k)append(&w,"%s%.9g",k?",":"",(double)p->psd[k]);
    append(&w,"],\"bands\":[");
    if(s->has_spectrum)for(size_t i=0;i<LFN_SPECTRUM_BANDS;++i) {
        const LfnSpectrumBand *b=&p->bands[i];
        append(&w,"%s{\"nominal_center_hz\":%.12g,\"exact_center_hz\":%.12g,\"low_hz\":%.12g,\"high_hz\":%.12g,\"power\":%.12g,\"dbfs\":",
               i?",":"",b->nominal_center_hz,b->exact_center_hz,b->low_hz,b->high_hz,b->power);
        level(&w,b->level);append(&w,"}");
    }
    append(&w,"],\"peaks\":[");
    if(s->has_spectrum)for(size_t i=0;i<p->peak_count;++i) {
        const LfnSpectrumPeak *peak=&p->peaks[i];
        append(&w,"%s{\"bin\":%u,\"frequency_hz\":%.12g,\"power\":%.12g,\"dbfs\":",i?",":"",peak->bin,peak->frequency_hz,peak->main_lobe_power);
        level(&w,peak->level);append(&w,"}");
    }
    append(&w,"],\"health\":{\"accepted\":");u64(&w,s->health.accepted);
    append(&w,",\"rejected_packet\":");u64(&w,s->health.rejected_packet);
    append(&w,",\"rejected_identity\":");u64(&w,s->health.rejected_identity);
    append(&w,",\"rejected_order\":");u64(&w,s->health.rejected_order);
    append(&w,",\"missing_frames\":");u64(&w,s->health.missing_frames);
    append(&w,",\"max_core_ns\":");u64(&w,s->health.max_core_ns);
    append(&w,",\"max_step_ns\":");u64(&w,s->health.max_step_ns);
    append(&w,",\"ignored_raw\":");u64(&w,s->health.ignored_raw);
    append(&w,",\"pool_busy\":");u64(&w,s->health.pool_busy);
    append(&w,",\"release_errors\":");u64(&w,s->health.release_errors);
    append(&w,",\"queue_highwater\":%u}}",s->health.queue_highwater);
    if(!w.ok){out[0]=0;return false;}
    *written=w.length;return true;
}
