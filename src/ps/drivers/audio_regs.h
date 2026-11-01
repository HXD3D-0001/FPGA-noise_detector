#ifndef LFN_AUDIO_REGS_H
#define LFN_AUDIO_REGS_H
#include "pl_regs.h"
/* Dedicated optional ALC1 slave, base selected from a matching BSP. No guessed address.
 * Single owner. Timeout never authorizes another command or DMA/capture reuse. */
typedef struct {uint32_t seq,flags,schema,config_id;uint64_t capture_id,adc_count,test_count,latest_raw,fault_count,age_ticks;} PlAudioSnapshot;
enum {PL_AUDIO_RESTART=1,PL_AUDIO_CLEAR_FAULT=2};
PlResult pl_audio_probe(const PlRegs *);
PlResult pl_audio_command(const PlRegs *,uint32_t kind);
PlResult pl_audio_snapshot(const PlRegs *,uint64_t max_age_ticks,PlAudioSnapshot *);
#endif
