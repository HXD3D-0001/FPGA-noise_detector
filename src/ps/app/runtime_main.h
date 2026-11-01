#ifndef LFN_RUNTIME_MAIN_H
#define LFN_RUNTIME_MAIN_H
#include "../capture/capture_task.h"
/* Platform integration may queue a TEST request only after genuine DDR/CPU/
 * clock/cache/GIC startup validation. No request is queued automatically. The
 * capture task alone executes bounded STOP/COMMIT/snapshot/arm/START and exact
 * DMA action ACKs. API acceptance means queued; inspect runtime health state
 * for completion. STOP/RESET commands do not grant board execution authority. */
bool lfn_runtime_request_test(uint64_t session_id,uint32_t config_id);
bool lfn_runtime_request_command(uint32_t command);
/* Sole capture executor's inner ring preparation, never a caller bypass. */
bool lfn_runtime_prepare_capture(const CaptureConfig *);
#endif
