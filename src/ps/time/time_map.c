#include "time_map.h"
#include <math.h>
#include <float.h>
#include <string.h>

bool lfn_time_map_append(LfnTimeMap *m,const LfnTimeMapEntry *e) {
    if(!m || !e || m->count>=LFN_TIME_MAP_CAPACITY || !e->map_id || !e->session_id ||
       !e->capture_id || !e->config_id || !isfinite(e->raw_fs_hz) ||
       e->raw_fs_hz<1 || e->raw_fs_hz>1000000 || !isfinite(e->fs_uncertainty_ppm) ||
       e->fs_uncertainty_ppm<0 || e->fs_uncertainty_ppm>1000000)return false;
    if(m->count && e->map_id<=m->entries[m->count-1].map_id)return false;
    for(size_t i=0;i<m->count;++i) {
        const LfnTimeMapEntry *old=&m->entries[i];
        if(old->session_id==e->session_id && old->capture_id==e->capture_id &&
           (old->config_id!=e->config_id || e->valid_from_raw<=old->valid_from_raw))return false;
    }
    m->entries[m->count++]=*e;return true;
}
bool lfn_time_to_utc(const LfnTimeMap *m,LfnSampleStamp s,LfnUtcEstimate *out) {
    if(out)memset(out,0,sizeof(*out));
    if(!m || !out || m->count>LFN_TIME_MAP_CAPACITY || !s.session_id || !s.capture_id || !s.config_id)return false;
    const LfnTimeMapEntry *chosen=NULL;
    for(size_t i=0;i<m->count;++i) {
        const LfnTimeMapEntry *e=&m->entries[i];
        if(e->session_id==s.session_id && e->capture_id==s.capture_id && e->config_id==s.config_id &&
           e->valid_from_raw<=s.raw_index && (!chosen || e->valid_from_raw>chosen->valid_from_raw))chosen=e;
    }
    if(!chosen)return false;
    /* Subtract integers FIRST. Two raw indices above 2^53 can differ by
     * one tick although their separate double conversions are identical. */
    uint64_t ticks=s.raw_index-chosen->valid_from_raw;
    double elapsed=(double)ticks/chosen->raw_fs_hz*1000000000.0;
    double rounded=floor(elapsed+0.5);
    if(!isfinite(rounded) || rounded>=9223372036854775808.0)return false;
    int64_t offset=(int64_t)rounded;
    if(chosen->utc_at_valid_from_ns>INT64_MAX-offset)return false;
    /* Integer-to-double, division/multiply and nearest-ns rounding are
     * conservative bounds in addition to explicit Fs drift uncertainty. */
    double error=ceil(elapsed*chosen->fs_uncertainty_ppm/1000000.0+
                      elapsed*(4*DBL_EPSILON)+0.5);
    if(!isfinite(error) || error>=18446744073709551616.0)return false;
    uint64_t drift=(uint64_t)error;
    if(chosen->base_uncertainty_ns>UINT64_MAX-drift)return false;
    out->utc_ns=chosen->utc_at_valid_from_ns+offset;out->map_id=chosen->map_id;
    out->uncertainty_ns=chosen->base_uncertainty_ns+drift;return true;
}
bool lfn_stamp_compensate_group_delay(LfnSampleStamp s,LfnSampleStamp *out) {
    if(!out || s.group_delay_compensated || s.raw_index<LFN_GROUP_DELAY_RAW_TICKS ||
       !s.session_id || !s.capture_id || !s.config_id)return false;
    s.raw_index-=LFN_GROUP_DELAY_RAW_TICKS;s.group_delay_compensated=true;*out=s;return true;
}
bool lfn_u64_decimal(uint64_t n,char *destination,size_t capacity) {
    if(!destination || !capacity)return false;
    char reversed[20];size_t digits=0;
    do { reversed[digits++]=(char)('0'+n%10);n/=10; } while(n);
    if(capacity<=digits){destination[0]=0;return false;}
    for(size_t i=0;i<digits;++i)destination[i]=reversed[digits-i-1];
    destination[digits]=0;return true;
}
bool lfn_monotonic_coverage_tick(LfnMonotonicCoverage *c,uint64_t now,bool monitor,bool available) {
    if(!c || (available && !monitor) || (c->have_previous && now<c->last_monotonic_ns))return false;
    if(c->have_previous && c->monitor_requested) {
        uint64_t delta=now-c->last_monotonic_ns;
        if(delta>UINT64_MAX-c->monitor_requested_ns ||
           (c->available && delta>UINT64_MAX-c->available_ns))return false;
        c->monitor_requested_ns+=delta;
        if(c->available)c->available_ns+=delta;
    }
    c->last_monotonic_ns=now;c->have_previous=true;
    c->monitor_requested=monitor;c->available=available;return true;
}
bool lfn_range_coverage_add(LfnRangeCoverage *c,LfnSampleStamp first,uint64_t end) {
    if(!c || !first.session_id || !first.capture_id || !first.config_id ||
       first.group_delay_compensated || end<=first.raw_index)return false;
    uint64_t additional=end-first.raw_index;
    if(c->have_previous) {
        if(first.session_id!=c->session_id || first.capture_id<c->capture_id)return false;
        if(first.capture_id==c->capture_id) {
            if(first.config_id!=c->config_id || end<c->end_raw)return false;
            if(first.raw_index<c->end_raw)additional=end-c->end_raw;
        }
    }
    if(additional>UINT64_MAX-c->valid_raw_ticks)return false;
    c->valid_raw_ticks+=additional;c->have_previous=true;c->session_id=first.session_id;
    c->capture_id=first.capture_id;c->config_id=first.config_id;c->end_raw=end;return true;
}
