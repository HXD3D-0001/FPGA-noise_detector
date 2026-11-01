#ifndef LFN_SPECTRUM_H
#define LFN_SPECTRUM_H
#include "../../shared/protocol.h"

enum { LFN_SPECTRUM_BINS=1311, LFN_SPECTRUM_BANDS=10,
       LFN_SPECTRUM_MAX_PEAKS=8 };
typedef enum { LFN_LEVEL_VALID, LFN_LEVEL_ZERO, LFN_LEVEL_BELOW_FLOOR,
               LFN_LEVEL_INVALID } LfnLevelStatus;
typedef struct { double dbfs; LfnLevelStatus status; } LfnLevel;
typedef struct {
    PacketHeader header;
    uint64_t session_id;
    const int32_t *interleaved;
    size_t value_count;
} LfnFftBlock;
typedef struct {
    uint32_t config_id;
    double nominal_fs_hz, measured_fs_hz;
    bool measured_fs_valid, test_source;
} LfnAnalysisConfig;
typedef struct { double nominal_center_hz, exact_center_hz, low_hz, high_hz,
                       power; LfnLevel level; } LfnSpectrumBand;
typedef struct { uint16_t bin; double frequency_hz, main_lobe_power;
                 LfnLevel level; } LfnSpectrumPeak;
typedef struct {
    uint64_t session_id, capture_id, stream_seq, first_raw, end_raw;
    uint32_t config_id;
    uint16_t flags;
    uint8_t blk_exp;
    bool valid, test_source, measured_fs_valid;
    /* Always relative full-scale power/Hz, never calibrated Pa^2/Hz. */
    double nominal_fs_hz, measured_fs_hz, fs_used_hz, bin_spacing_hz;
    double observation_seconds, refresh_seconds, window_sum_squares;
    float psd[LFN_SPECTRUM_BINS];
    double full_range_power, target_power;
    LfnLevel full_range_level, target_level;
    LfnSpectrumBand bands[LFN_SPECTRUM_BANDS];
    size_t peak_count;
    LfnSpectrumPeak peaks[LFN_SPECTRUM_MAX_PEAKS];
} LfnSpectrum;

/* Exact energy of generated/hann4096.mem, normalized signed Q1.17.
 * No dynamic allocation; caller owns both input and independent output. */
double lfn_hann_sum_squares(void);
bool lfn_spectrum_compute(const LfnFftBlock *,const LfnAnalysisConfig *,LfnSpectrum *);
/* The PacketView must have passed packet_decode; payload is borrowed only
 * during this call, and no pointer survives in the independent result. */
bool lfn_spectrum_from_packet(uint64_t session_id,const PacketView *,
                              const LfnAnalysisConfig *,LfnSpectrum *);
/* Archive length is strict: legacy 1229-bin arrays are rejected. Endpoint
 * cells own full df contributions (DC/Nyquist are not doubled). */
bool lfn_spectrum_band_power(const float *,size_t count,double fs_hz,
                              double low_hz,double high_hz,double *power);
LfnLevel lfn_power_level(double power,double floor_dbfs);
#endif
