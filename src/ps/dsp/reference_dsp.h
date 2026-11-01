#ifndef LFN_REFERENCE_DSP_H
#define LFN_REFERENCE_DSP_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef struct { int32_t value; uint64_t capture_id,raw_anchor; uint8_t flags; } LfnSample;
typedef struct {
 const int32_t *coefficients;
 size_t taps,factor,phase;
 uint64_t anchor_step,saturation_count;
 int32_t history[67]; uint8_t flag_history[67];
 bool have_previous; uint64_t previous_capture,previous_anchor;
} LfnFir;
typedef struct {
 LfnFir stages[3]; bool have_previous;
 uint64_t previous_capture,previous_anchor; unsigned output_index;
} LfnDecimator;
/* FIR accumulators must fit signed56. Input samples are signed24.
 * Coefficients are borrowed for the entire state lifetime, signed Q1.17. */
int64_t lfn_round_q17(int64_t accumulator);
int32_t lfn_saturate24(int64_t value,uint8_t *flags,uint64_t *counter);
bool lfn_fir_init(LfnFir *state,const int32_t *coefficients,size_t taps,size_t factor,uint64_t anchor_step);
void lfn_fir_clear(LfnFir *state);
bool lfn_fir_push(LfnFir *state,const LfnSample *input,LfnSample *output);
/* GAP at raw chain ingress clears all stage history before accepting that
 * same sample, including when capture ID and raw anchor remain continuous.
 * Individual FIRs clear on capture/index gaps or explicit clear; their ORed
 * flags are metadata, never another request to reset a later FIR stage. */
void lfn_decimator_init(LfnDecimator *state);
void lfn_decimator_clear(LfnDecimator *state);
bool lfn_decimator_push(LfnDecimator *state,const LfnSample *input,LfnSample *output);
#endif
