#ifndef LFN_CAPTURE_APPLICATION_H
#define LFN_CAPTURE_APPLICATION_H
#include "capture_runtime.h"
#include "../capture/history.h"
#include "analysis_service.h"
#include "service_bridge.h"
#include "raw_capture.h"
#include "capture_storage_route.h"

typedef enum { CA_WAIT,CA_PROBE,CA_CLOCK,CA_BIND_DMA,CA_ACTIVE,CA_FAULT,CA_BOOT_PROOF } CaptureApplicationState;
typedef struct {
 void *context;
 uint64_t (*now_us)(void *);
 void (*notify)(void *);
 /* Initial binding/reset/IRQ setup only after source-off and a completed clock
  * ticket. Subsequent resets/arm operations belong to CaptureDataOwner. */
 bool (*bind_dma)(void *);
 bool (*halt)(void *,unsigned),(*idle)(void *,unsigned),(*rearm)(void *,unsigned);
 uint32_t (*fault_mask)(void *);
 /* Before binding: read actual HALTED/RS/reset/error state of both DMAs.
  * No reset, descriptor allocation, IRQ setup or buffer ownership changes. */
 bool (*boot_dma_idle)(void *);
} CaptureApplicationHooks;
typedef struct {CaptureRuntimeLimits runtime;uint64_t owner_us;LfnRawPolicy raw;} CaptureApplicationLimits;
typedef struct {
 CaptureApplicationState state;PlResult failure;PlRegs cap,dat,ctr;
 CaptureApplicationHooks hooks;CaptureApplicationLimits limits;
 CaptureRuntime runtime;CaptureDataOwner owner;CaptureTask task;LfnAnalysisService analysis;
 BlockPool *pool;DmaRx *flows[2];PlCaptureConfig selected;uint64_t session,since,last_now;
 PlCaptureTicket clock_ticket;bool clock_pending,clock_may_clear,cancel_start,bound,prepared,plan_valid,have_time;
 uint32_t boot_ticket;bool boot_proof_done,boot_pending,boot_acked;PlDataAction boot_action;
 PcmHistory *history;LfnRawCapture *raw;CaptureStorageRoute *storage;bool storage_close_requested,storage_input_closed;uint64_t storage_generation;
 LfnServiceBridge *services;PlCaptureConfig active_config;bool accepted_capture;
 uint32_t soft_fault;uint64_t packets[3],energy_records,energy_gaps;CaptureEnergyMessage last_energy;
} CaptureApplication;
/* Pure, single-owner initialization. Pool must already be initialized; no IO,
 * scheduler, DMA, clock, START, DDR initialization or implicit session here. */
bool capture_application_init(CaptureApplication *,const PlRegs *,const PlRegs *,const PlRegs *,
 DmaRx *,DmaRx *,BlockPool *,const CaptureApplicationHooks *);
/* Explicit software policy deadlines; zero values are rejected. Caller chooses
 * limits for the actual hardware/scheduler, never Host accelerated values. */
bool capture_application_request_start(CaptureApplication *,uint64_t,const PlCaptureConfig *,const CaptureApplicationLimits *);
bool capture_application_request_stop(CaptureApplication *,bool discard);
/* Attach only before first request/bind; bridge lives beyond the application.
 * Downstream backlog holds copies, not DMA, and does not block STOP/new IDs. */
bool capture_application_attach_services(CaptureApplication *,LfnServiceBridge *);
/* RAW requests require attached fixed storage and both explicit limits. */
bool capture_application_attach_history(CaptureApplication *,PcmHistory *);
bool capture_application_attach_raw(CaptureApplication *,LfnRawCapture *);
bool capture_application_attach_storage(CaptureApplication *,CaptureStorageRoute *);
StorageResult capture_application_attach_storage_result(CaptureApplication *,CaptureStorageRoute *);
bool capture_application_close_storage(CaptureApplication *);
/* Capture owner only, after storage owner has stopped stepping and successfully
 * unmounted. Retires the drained route without resetting DMA/clock ownership. */
StorageResult capture_application_detach_storage(CaptureApplication *);
/* Capture owner, both owners paused after storage reconnect. Normal STOPPED
 * only (never FAILED), exact new route generation; no MMIO/START. */
bool capture_application_resume_storage(CaptureApplication *,uint64_t generation);
void capture_application_step(CaptureApplication *);
#endif
