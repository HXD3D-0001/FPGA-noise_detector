#ifndef LFN_CAPTURE_DATA_H
#define LFN_CAPTURE_DATA_H
#include "pl_regs.h"
#include "dma_rx.h"
enum {PL_DATA_DRAIN=1,PL_DATA_RESET=2,PL_DATA_FLOW_SPECTRUM=3,PL_DATA_FLOW_PCM=4};
typedef struct {uint32_t seq,mask;bool reset,resume;} PlDataAction;
PlResult pl_data_probe(const PlRegs *);
PlResult pl_data_request(const PlRegs *,uint32_t kind,uint32_t *seq);
PlResult pl_data_poll(const PlRegs *,uint32_t seq,uint32_t *status);
PlResult pl_data_action(const PlRegs *,PlDataAction *);
PlResult pl_data_ack(const PlRegs *,const PlDataAction *);
PlResult pl_data_energy_read(const PlRegs *,PlEnergy *);
/* Task serialized with CaptureTask. halt/idle refer to actual hardware (e.g.
 * dma_rx_xilinx_halt/idle), never token states. rearm prepares real descriptors
 * after reset completion and before selected producer flush ACK. */
typedef struct {
 void *context;
 bool (*halt)(void *,unsigned);
 bool (*idle)(void *,unsigned);
 bool (*rearm)(void *,unsigned);
} CaptureDataHooks;
typedef struct {
 PlRegs regs;DmaRx *flows[2];CaptureDataHooks hooks;PlDataAction action;
 uint64_t since,limit_us;uint32_t started,done;
 bool active,ack_pending,timed_out;PlResult failure;
} CaptureDataOwner;
bool capture_data_owner_init(CaptureDataOwner *,const PlRegs *,DmaRx *,DmaRx *,const CaptureDataHooks *,uint64_t limit_us);
/* One bounded action pass. Caller still runs CaptureTask/analysis/energy each
 * scheduler turn. TIMEOUT retains state and may accept late genuine proof;
 * failure never invokes reset completion or frees memory by itself. */
PlResult capture_data_owner_step(CaptureDataOwner *,uint64_t now_us);
#endif
