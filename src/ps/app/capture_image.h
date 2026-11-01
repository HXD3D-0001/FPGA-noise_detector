#ifndef LFN_CAPTURE_IMAGE_H
#define LFN_CAPTURE_IMAGE_H
#include "capture_application.h"
#include "capture_control.h"
#include "capture_request.h"
#include "pre_adc_controller.h"
/* Task-context mailbox only: true means queued, not running or completed.
 * Caller must establish a real session/configuration and explicit limits. */
bool lfn_capture_image_request_start(uint64_t,const PlCaptureConfig *,const CaptureApplicationLimits *);
bool lfn_capture_image_request_stop(bool discard);
/* Exact retained execution result. Submission is only QUEUED; receipt.result
 * STORE_OK is application acceptance, NOT RUN/STOPPED. ACK START to admit a
 * subsequent STOP, including cancellation while initialization is pending. */
StorageResult lfn_capture_image_request_submit_start(uint64_t,const PlCaptureConfig *,const CaptureApplicationLimits *,uint64_t *ticket);
StorageResult lfn_capture_image_request_submit_stop(bool discard,uint64_t *ticket);
StorageResult lfn_capture_image_request_copy(uint64_t,CaptureRequestReceipt *);
StorageResult lfn_capture_image_request_ack(uint64_t);
/* Caller-owned copies. Nonblocking task-context reads, never FatFS/network IO.
 * A successful dequeue means delivered to this consumer, not persisted. */
bool lfn_capture_image_storage_attach(CaptureStorageRoute *);
bool lfn_capture_image_storage_close(void);
LfnServiceResult lfn_capture_image_snapshot_copy(LfnServiceSnapshot *);
LfnServiceResult lfn_capture_image_energy_take(LfnServiceMessage *);
/* Complete retained RAW packet, copied under a bounded try-lock. Exact
 * identity required; new RAW prepare invalidates the previous RAM capture. */
LfnRawResult lfn_capture_image_raw_read(uint64_t,uint64_t,uint32_t,uint64_t,uint8_t *,size_t);
/* Task-context retained command result; enqueue success is NOT PAUSE proof.
 * STOP_CLOSE(0,0) requests normal stop + capture input close and retains its
 * exact result. Storage must continue draining independently. Only after
 * route handoff submit PAUSE, poll exact accepted receipt, ACK it.
 * Barrier persists until exact RESUME after storage mutations are finished. */
StorageResult lfn_capture_image_control_submit(CaptureControlKind,uint64_t pause,uint64_t generation,uint64_t *);
StorageResult lfn_capture_image_control_attach(CaptureStorageRoute *,uint64_t *ticket);
StorageResult lfn_capture_image_control_copy(uint64_t,CaptureControlReceipt *);
StorageResult lfn_capture_image_control_ack(uint64_t);
bool lfn_capture_image_control_barrier(CaptureControlBarrier *);
/* P2 pre-ADC controller: dormant until a real host session token begins it.
 * begin succeeds at most once per boot; no invented session and no auto
 * START. begin/command/status belong to the capture owner task, mirroring
 * the request mailbox. diag_take drains the controller-owned bounded ring;
 * a separate future UART task must add external serialization around it. */
bool lfn_capture_image_pre_adc_begin(uint64_t host_session);
/* True after the previous session has fully stopped and storage teardown/
 * detach completed; a second host BEGIN may then start a new session. */
bool lfn_capture_image_can_rebegin(uint64_t host_session);
PreAdcResult lfn_capture_image_pre_adc_command(const PreAdcCommand *);
bool lfn_capture_image_pre_adc_status(PreAdcStatus *);
bool lfn_capture_image_pre_adc_diag_take(PreAdcDiagEvent *);
#endif
extern uint32_t lfn_capture_image_status_word(void);