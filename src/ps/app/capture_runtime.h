#ifndef LFN_CAPTURE_RUNTIME_H
#define LFN_CAPTURE_RUNTIME_H
#include "../drivers/capture_regs.h"
#include "../drivers/capture_contract.h"
#include "../drivers/capture_data.h"
#include "capture_time_pair.h"

typedef enum { CR_WAIT,CR_PROBE,CR_CLOSING,CR_COMMIT,CR_PLAN,CR_ARM,
 CR_START,CR_BIND,CR_RUN,CR_STOPPED,CR_FAILED,CR_INCOMPATIBLE,CR_QUARANTINED } CaptureRuntimeState;
typedef struct {
 uint64_t transaction_us,bind_us,close_us,snapshot_age_ticks;
} CaptureRuntimeLimits;
typedef struct {
 void *context;
 uint64_t (*now_us)(void *);
 /* Called once per new PLAN with all hardware/software owners quiesced.
  * Initialize/prepare the real CaptureTask and matching analysis identity.
  * No DMA submissions here. False must leave external analysis quiescent. */
 bool (*prepare)(void *,const CapturePlan *);
 void (*analysis_step)(void *); /* bounded; consumes copied data + energy/gaps */
 bool (*analysis_idle)(void *); /* includes retained analysis/release work */
 uint32_t (*fault_mask)(void *); /* actual backend faults, not cleared here */
} CaptureRuntimeHooks;
typedef struct {
 PlRegs control,contract;CaptureTask *capture;CaptureDataOwner *owner;
 CaptureRuntimeHooks hooks;CaptureRuntimeLimits limits;CaptureRuntimeState state;
 PlCaptureConfig requested;PlContractSnapshot prepared;
 PlCaptureSnapshot observed_capture,observed_started;PlContractSnapshot observed_active;bool have_observed_capture,have_observed_active,have_observed_started;
 CaptureTimeTracker time_tracker;
 uint64_t session_id,since,last_now,cap_stale_since,contract_stale_since;
 bool have_time,verified,capture_ready,cap_stale,contract_stale;
 PlCaptureTicket cap_ticket;uint32_t cap_command;bool cap_pending,phase_sent;
 uint32_t data_ticket,data_kind;bool data_pending,drain_done,reset_done;
 /* Frozen before DMA cleanup, never inferred from a later observed snapshot. */
 PlCaptureSnapshot final_capture;uint64_t final_session;
 uint32_t drain_action_before,final_drain_seq,final_drain_ticket;
 bool final_pending,final_clean;
 bool continue_start,need_reset,quarantined,uncertain,reset_failed;
 unsigned arm_flow;PlResult failure;
 uint64_t closures,discard_closures,reject_watch[CAPTURE_REASON_COUNT],energy_watch;
} CaptureRuntime;
/* Pure initialization: caller already bound real DmaRx/owner backends after
 * platform setup. No DDR/CPU/interrupt setup or MMIO happens here. One task
 * alone calls request/step and owns CAP1/DAT1/CTR1 + CaptureTask/energy. */
bool capture_runtime_init(CaptureRuntime *,const PlRegs *control,const PlRegs *contract,
 CaptureTask *,CaptureDataOwner *,const CaptureRuntimeHooks *,const CaptureRuntimeLimits *);
bool capture_runtime_request_start(CaptureRuntime *,uint64_t session_id,const PlCaptureConfig *);
/* During an issued CAP1 operation retain its ticket until actual completion.
 * discard selects RESET after source-off; it never replaces a pending DRAIN. */
bool capture_runtime_request_stop(CaptureRuntime *,bool discard);
/* One bounded cooperative scheduler pass. WAIT/STOPPED/safely FAILED are idle.
 * CR_CLOSING + quarantined is live: continue stepping for late real proofs.
 * CR_FAILED means verified STOP + reset + software drain. CR_INCOMPATIBLE
 * means rejected ABI before ownership; it is NOT a hardware-safe certificate.
 * CR_QUARANTINED is terminal: stop proof can never complete (a real backend
 * fault makes the DRAIN idle proof impossible while its DAT1 action is
 * already in flight). Ownership is retained, no clean state is claimed, and
 * only a restart recovers. */
void capture_runtime_step(CaptureRuntime *);
#endif
