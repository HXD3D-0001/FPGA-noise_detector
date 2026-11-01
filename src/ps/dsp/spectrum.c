#include "spectrum.h"
#include <math.h>
#include <string.h>

double lfn_hann_sum_squares(void) {
    /* Sum of squares of all 4096 committed integer coefficients. This is
     * deliberately distinct from ideal Hann energy 1536. Gold validation
     * checks the ROM hash and computes this sum independently. */
    return 26388278898841.0 / 17179869184.0;
}
LfnLevel lfn_power_level(double power,double floor_dbfs) {
    LfnLevel r={0.0,LFN_LEVEL_INVALID};
    if(!isfinite(power) || power<0 || !isfinite(floor_dbfs))return r;
    if(power==0){r.status=LFN_LEVEL_ZERO;return r;}
    /* 0 dBFS power reference is a full-scale sinusoid's mean square 0.5.
     * Subtract logs to avoid overflowing power/0.5 for arbitrary callers. */
    double db=10.0*(log10(power)-log10(0.5));
    if(db<floor_dbfs){r.status=LFN_LEVEL_BELOW_FLOOR;return r;}
    r.dbfs=db;r.status=LFN_LEVEL_VALID;return r;
}

bool lfn_spectrum_band_power(const float *psd,size_t count,double fs,
                              double low,double high,double *power) {
    if(power)*power=0;
    if(!psd || !power || count!=LFN_SPECTRUM_BINS || !isfinite(fs) || fs<1 || fs>48000 ||
       !isfinite(low) || !isfinite(high) || low<0 || high<=low)return false;
    const double df=fs/LFN_FFT_COUNT;
    if(high>((double)(count-1)+0.5)*df)return false;
    double sum=0;
    for(size_t k=0;k<count;++k) {
        if(!isfinite(psd[k]) || psd[k]<0)return false;
        double left=k?((double)k-0.5)*df:0;
        double right=((double)k+0.5)*df;
        double overlap=fmin(high,right)-fmax(low,left);
        /* DC has a half-width cell, but its total PSD*df contribution is
         * still full width to retain Parseval. No double-sided energy. */
        if(overlap>0)sum+=(double)psd[k]*df*overlap/(right-left);
    }
    *power=sum;return true;
}

static int32_t read_value(const int32_t *values,const PacketView *packet,size_t i) {
    int32_t result=0;
    if(values)return values[i];
    (void)packet_value(packet,i,&result);return result;
}
static bool compute(uint64_t session,const PacketHeader *h,const int32_t *values,
                     const PacketView *packet,size_t count,const LfnAnalysisConfig *cfg,
                     LfnSpectrum *out) {
    if(out)memset(out,0,sizeof(*out));
    if(!out || !cfg || !h || !session || (!values && !packet) || count!=8192 ||
       h->type!=LFN_FFT || !h->capture_id || !h->config_id ||
       h->config_id!=cfg->config_id || h->blk_exp>12 || (h->flags&~UINT16_C(63)) ||
       h->first_raw_sample_index>UINT64_MAX-UINT64_C(196608) ||
       !isfinite(cfg->nominal_fs_hz) || cfg->nominal_fs_hz<1 || cfg->nominal_fs_hz>48000 ||
       (cfg->measured_fs_valid && (!isfinite(cfg->measured_fs_hz) ||
        cfg->measured_fs_hz<1 || cfg->measured_fs_hz>48000)))return false;
    double fs=cfg->measured_fs_valid?cfg->measured_fs_hz:cfg->nominal_fs_hz;
    /* All defined target bands must fit the archive; an arbitrary rate is
     * not permitted to silently turn truncated bins into full-band power. */
    if(281.83829312644537>((LFN_SPECTRUM_BINS-1)+0.5)*fs/LFN_FFT_COUNT)return false;
    for(size_t i=0;i<count;++i) {
        int32_t v=read_value(values,packet,i);
        if(v<-8388608 || v>8388607)return false;
    }
    out->session_id=session;out->capture_id=h->capture_id;out->stream_seq=h->stream_seq;
    out->first_raw=h->first_raw_sample_index;out->end_raw=out->first_raw+196608;
    out->config_id=h->config_id;out->flags=h->flags;out->blk_exp=h->blk_exp;
    out->test_source=cfg->test_source;out->measured_fs_valid=cfg->measured_fs_valid;
    out->nominal_fs_hz=cfg->nominal_fs_hz;
    out->measured_fs_hz=cfg->measured_fs_valid?cfg->measured_fs_hz:0;
    out->fs_used_hz=fs;out->bin_spacing_hz=fs/LFN_FFT_COUNT;
    out->observation_seconds=LFN_FFT_COUNT/fs;out->refresh_seconds=2048/fs;
    out->window_sum_squares=lfn_hann_sum_squares();
    out->valid=(h->flags&LFN_VALID) && !(h->flags&(LFN_WARMUP|LFN_GAP|LFN_SATURATION|
                                                        LFN_PROTOCOL_ERROR|LFN_CLOCK_ERROR));
    const double scale=ldexp(1.0,(int)h->blk_exp-23);
    for(size_t k=0;k<=LFN_FFT_COUNT/2;++k) {
        double re=(double)read_value(values,packet,2*k)*scale;
        double im=(double)read_value(values,packet,2*k+1)*scale;
        double psd=(re*re+im*im)/(fs*out->window_sum_squares);
        if(k && k!=LFN_FFT_COUNT/2)psd*=2.0;
        out->full_range_power+=psd*out->bin_spacing_hz;
        if(k<LFN_SPECTRUM_BINS)out->psd[k]=(float)psd;
    }
    (void)lfn_spectrum_band_power(out->psd,LFN_SPECTRUM_BINS,fs,31.5,250,&out->target_power);
    out->full_range_level=lfn_power_level(out->full_range_power,-120);
    out->target_level=lfn_power_level(out->target_power,-120);
    const double nominal[10]={31.5,40,50,63,80,100,125,160,200,250};
    for(size_t i=0;i<LFN_SPECTRUM_BANDS;++i) {
        LfnSpectrumBand *b=&out->bands[i];
        b->nominal_center_hz=nominal[i];
        b->exact_center_hz=1000*pow(10.0,((double)i-15)/10);
        b->low_hz=b->exact_center_hz/pow(10.0,0.05);
        b->high_hz=b->exact_center_hz*pow(10.0,0.05);
        (void)lfn_spectrum_band_power(out->psd,LFN_SPECTRUM_BINS,fs,b->low_hz,b->high_hz,&b->power);
        b->level=lfn_power_level(b->power,-120);
    }
    for(size_t k=1;k+1<LFN_SPECTRUM_BINS;++k) {
        double f=k*out->bin_spacing_hz;
        if(f<31.5-out->bin_spacing_hz/2 || f>250+out->bin_spacing_hz/2 ||
           out->psd[k]<=out->psd[k-1] || out->psd[k]<out->psd[k+1])continue;
        LfnSpectrumPeak candidate;candidate.bin=(uint16_t)k;candidate.frequency_hz=f;
        double low=((double)k-2)*out->bin_spacing_hz;
        double high=((double)k+2)*out->bin_spacing_hz;
        (void)lfn_spectrum_band_power(out->psd,LFN_SPECTRUM_BINS,fs,low,high,&candidate.main_lobe_power);
        candidate.level=lfn_power_level(candidate.main_lobe_power,-120);
        size_t position=0;
        while(position<out->peak_count &&
              out->psd[out->peaks[position].bin]>=out->psd[k])++position;
        if(position>=LFN_SPECTRUM_MAX_PEAKS)continue;
        size_t end=out->peak_count<LFN_SPECTRUM_MAX_PEAKS?out->peak_count:LFN_SPECTRUM_MAX_PEAKS-1;
        for(size_t j=end;j>position;--j)out->peaks[j]=out->peaks[j-1];
        out->peaks[position]=candidate;
        if(out->peak_count<LFN_SPECTRUM_MAX_PEAKS)++out->peak_count;
    }
    return true;
}
bool lfn_spectrum_compute(const LfnFftBlock *b,const LfnAnalysisConfig *c,LfnSpectrum *o) {
    if(!b){if(o)memset(o,0,sizeof(*o));return false;}
    return compute(b->session_id,&b->header,b->interleaved,NULL,b->value_count,c,o);
}
bool lfn_spectrum_from_packet(uint64_t session,const PacketView *p,const LfnAnalysisConfig *c,LfnSpectrum *o) {
    if(!p || !p->payload || p->payload_bytes!=32768 || p->source_sample_count!=4096 ||
       p->raw_ticks_per_sample!=48 || p->header.first_raw_sample_index>UINT64_MAX-196608 ||
       p->end_raw!=p->header.first_raw_sample_index+196608) {
        if(o)memset(o,0,sizeof(*o));
        return false;
    }
    return compute(session,&p->header,NULL,p,p->value_count,c,o);
}
