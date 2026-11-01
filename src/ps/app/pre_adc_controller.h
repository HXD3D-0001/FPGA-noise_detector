#ifndef LFN_PRE_ADC_CONTROLLER_H
#define LFN_PRE_ADC_CONTROLLER_H
#include "capture_request.h"
#include "pre_adc_diag.h"
typedef enum { PRE_ADC_START_TEST=1,PRE_ADC_STOP,PRE_ADC_STATUS,PRE_ADC_RECOVER,PRE_ADC_START_ADC } PreAdcCommandKind;
typedef enum { PRE_ADC_IDLE,PRE_ADC_STARTING,PRE_ADC_RUNNING,PRE_ADC_STOPPING,PRE_ADC_STOPPED,PRE_ADC_FAULT } PreAdcState;
typedef enum { PRE_ADC_NONE,PRE_ADC_QUEUED,PRE_ADC_ACCEPTED,PRE_ADC_RUN_CONFIRMED,PRE_ADC_STOP_CONFIRMED,
 PRE_ADC_RAW_COMPLETE,PRE_ADC_CANCELLED,PRE_ADC_REJECTED,PRE_ADC_FAILED } PreAdcOutcome;
typedef enum { PRE_ADC_OK,PRE_ADC_DUPLICATE,PRE_ADC_BUSY,PRE_ADC_INVALID,PRE_ADC_STALE,PRE_ADC_UNSUPPORTED } PreAdcResult;
typedef struct {
 uint64_t id,session_id;PreAdcCommandKind kind;PlCaptureConfig config;CaptureApplicationLimits limits;bool discard;
} PreAdcCommand;
typedef struct {
 void *context;
 StorageResult (*submit_start)(void *,uint64_t,const PlCaptureConfig *,const CaptureApplicationLimits *,uint64_t *);
 StorageResult (*submit_stop)(void *,bool,uint64_t *);
 StorageResult (*request_copy)(void *,uint64_t,CaptureRequestReceipt *);
 StorageResult (*request_ack)(void *,uint64_t);
 LfnServiceResult (*snapshot_copy)(void *,LfnServiceSnapshot *);
} PreAdcHooks;
typedef struct {
 PreAdcState state;PreAdcOutcome start,stop;
 uint64_t session_id,start_command,stop_command,capture_id,snapshot_version;
 uint32_t config_id;int32_t failure;LfnRawStop raw_reason;
 bool fault_latched,have_snapshot;
} PreAdcStatus;
typedef struct {
 PreAdcHooks hooks;PreAdcDiag *diag;PreAdcStatus status;
 PreAdcCommand last,start_command,stop_command;
 uint64_t ticket,start_floor,stop_floor;uint32_t max_config;
 bool initialized,pending_start,pending_stop,request_is_start,receipt_accepted;
 bool start_accepted,stop_accepted,have_last;
 LfnServiceSnapshot snapshot;
} PreAdcController;
/* host_session is a nonzero, host-unique boot token, never an invented constant.
 * All APIs belong to one serialized controller task. IDs strictly increase;
 * exact retry of the last admitted command is idempotent. Older IDs are stale.
 * Configuration IDs also strictly increase within this boot token.
 * START kind must match config.source; ADC requires pattern=0. */
bool pre_adc_controller_init(PreAdcController *,uint64_t host_session,const PreAdcHooks *,PreAdcDiag *);
/* Capture owner only. A new token is admitted only after a confirmed normal
 * STOP and all retained request receipts have been acknowledged. Storage
 * teardown is an additional caller precondition, independent of this gate. */
bool pre_adc_controller_can_rebegin(const PreAdcController *,uint64_t host_session);
bool pre_adc_controller_rebegin(PreAdcController *,uint64_t host_session);
PreAdcResult pre_adc_controller_command(PreAdcController *,const PreAdcCommand *);
/* Capture owner only: continue the host START after storage rotation has
 * normally stopped the old capture. Only config_id may change. The new START
 * uses the existing request/receipt path; a pending host STOP takes priority. */
PreAdcResult pre_adc_controller_restart(PreAdcController *,const PlCaptureConfig *);
void pre_adc_controller_step(PreAdcController *);
/* Caller-owned copy; not a concurrent mailbox. Snapshot fields are evidence
 * from the last successfully copied publication, not an asserted fresh read. */
bool pre_adc_controller_status(const PreAdcController *,PreAdcStatus *);
#endif
