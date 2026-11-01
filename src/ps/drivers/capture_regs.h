#ifndef LFN_CAPTURE_REGS_H
#define LFN_CAPTURE_REGS_H
#include "pl_regs.h"
/* CAP1 base must come from the matching BSP. One task owns configuration,
 * capture and clock commands. Polls never reclaim DMA or cancel timed-out work. */
typedef struct {uint32_t mode,source,channel,pattern,config_id;} PlCaptureConfig;
typedef struct {uint32_t channel,seq;} PlCaptureTicket;
typedef struct {
 uint32_t seq,flags,active_config_id;PlCaptureConfig configured;
 uint64_t capture_id,adc_count,test_count,latest_raw,origin_raw,age_ticks;
} PlCaptureSnapshot;
enum {PL_CAPTURE_ADC=0,PL_CAPTURE_TEST=1};
enum {PL_CAPTURE_TX_COMMAND=0,PL_CAPTURE_TX_COMMIT=1};
enum {PL_CAPTURE_F_ARMED=1u,PL_CAPTURE_F_SERVICE_BUSY=2u,
 PL_CAPTURE_F_QUARANTINED=4u,PL_CAPTURE_F_RUNNING=8u,
 PL_CAPTURE_F_STOP_COMPLETE=16u,PL_CAPTURE_F_START_READY=32u,
 PL_CAPTURE_F_TEST=64u,PL_CAPTURE_F_POSITION_VALID=128u,
 PL_CAPTURE_F_CLOCK_BUSY=256u};
/* Snapshot flags[11:9]: actual service operation, distinct from command IDs. */
enum {PL_CAPTURE_OP_IDLE=0,PL_CAPTURE_OP_COMMIT=1,PL_CAPTURE_OP_START=2,
 PL_CAPTURE_OP_STOP=3,PL_CAPTURE_OP_RECOVER=4};
enum {PL_CAPTURE_START=1,PL_CAPTURE_STOP=2,PL_CAPTURE_CLOCK_RESTART=3,PL_CAPTURE_CLOCK_CLEAR=4};
PlResult pl_capture_probe(const PlRegs *);
PlResult pl_capture_command_request(const PlRegs *,uint32_t,PlCaptureTicket *);
PlResult pl_capture_commit_request(const PlRegs *,const PlCaptureConfig *,PlCaptureTicket *);
/* PL_TIMEOUT may coexist with BUSY. Retain the ticket and continue servicing
 * downstream owners; only a non-BUSY status means this transaction has ended.
 * status is written only after both tag reads match the supplied ticket. */
PlResult pl_capture_poll(const PlRegs *,const PlCaptureTicket *,uint32_t *status);
PlResult pl_capture_snapshot(const PlRegs *,uint64_t max_age_ticks,PlCaptureSnapshot *);
#endif
