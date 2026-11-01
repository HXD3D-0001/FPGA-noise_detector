#ifndef LFN_CAPTURE_CONTRACT_H
#define LFN_CAPTURE_CONTRACT_H
#include "../capture/capture_task.h"
/* Independent CTR1 base from matching BSP; same sole owner as CAP1/DAT1.
 * session is a software identity, never read from a made-up hardware register. */
enum { PL_CONTRACT_PLAN=1,PL_CONTRACT_ACTIVE=2 };
typedef struct {
 uint32_t seq,kind,mode,source,channel,pattern,bound_mask;
 uint64_t age_ticks;
 CapturePlan plan;
 CaptureAnchor anchors[4];
} PlContractSnapshot;
PlResult pl_contract_probe(const PlRegs *);
/* Bounded single snapshot transaction; any failure leaves output unchanged. */
PlResult pl_contract_read(const PlRegs *,uint32_t kind,uint64_t session,uint64_t max_age_ticks,PlContractSnapshot *);
/* Preflights all bound anchors before changing any; never touches DMA/queues.
 * Pending streams remain pending. Caller still owns timeout/STOP policy. */
PlResult pl_contract_bind(CaptureTask *,const PlContractSnapshot *);
#endif
