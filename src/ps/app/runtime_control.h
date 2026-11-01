#ifndef LFN_RUNTIME_CONTROL_H
#define LFN_RUNTIME_CONTROL_H
#include "../capture/capture_task.h"
enum { RUNTIME_CONTROL_LIMIT_US=100000 };
/* control_service state_i ABI: WARMUP and RUN are distinct from command bits. */
enum { RC_PL_STOPPED=0,RC_PL_WARMUP=4,RC_PL_RUN=5 };
typedef enum { RC_WAIT,RC_INIT,RC_STOP_SEND,RC_STOP_WAIT,RC_COMMIT_SEND,RC_COMMIT_WAIT,
 RC_SNAPSHOT_SEND,RC_SNAPSHOT_WAIT,RC_PREPARE,RC_START_SEND,RC_START_WAIT,RC_RUN,
 RC_USER_STOP_SEND,RC_USER_STOP_WAIT,RC_RESET_SEND,RC_RESET_WAIT,RC_STOPPED,RC_FAILED } RuntimeControlState;
typedef struct {
 void *context;
 uint64_t (*now_us)(void *);
 bool (*init_receivers)(void *);
 bool (*prepare_capture)(void *,const CaptureConfig *);
 void (*service_capture)(void *); /* bounded, sole DMA/energy consumer */
 bool (*halt_flow)(void *,unsigned);
 bool (*idle_flow)(void *,unsigned); /* actual RS0/RESET0/HALTED1, no release */
 bool (*reset_begin)(void *,unsigned); /* quarantines requested flow */
 bool (*reset_done)(void *,unsigned); /* actual hardware completion */
 bool (*rearm_flow)(void *,unsigned); /* after completion, before producer ACK */
 uint32_t (*fault_mask)(void *); /* actual backend/owner fault; never clear here */
} RuntimeControlHooks;
typedef struct {
 PlRegs regs;RuntimeControlHooks hooks;RuntimeControlState state;
 CaptureConfig config;PlDmaAction action;
 uint64_t state_since,action_since;
 uint32_t snapshot_before,action_started,action_done;
 uint32_t flow_seq,flow_mask;
 uint64_t flow_since,fault_since;
 PlResult failure;
 bool action_active,action_ack_pending,receivers_ready,capture_ready;
 bool flow_active,flow_timed_out,fault_waiting;
} RuntimeControl;
/* Task-serialized request/step API. This is application control, never board
 * execution authorization. Platform integration must establish the real DDR,
 * clocks, CPU/cache/GIC environment before invoking a request. Default WAIT
 * performs no MMIO. Every step performs bounded work without blocking polls. */
bool runtime_control_init(RuntimeControl *,const PlRegs *,const RuntimeControlHooks *);
bool runtime_control_request_test(RuntimeControl *,uint64_t session_id,uint32_t config_id);
bool runtime_control_request_command(RuntimeControl *,uint32_t command);
void runtime_control_step(RuntimeControl *);
#endif
