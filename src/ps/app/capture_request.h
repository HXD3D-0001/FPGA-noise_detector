#ifndef LFN_CAPTURE_REQUEST_H
#define LFN_CAPTURE_REQUEST_H
#include "capture_application.h"
typedef enum { REQUEST_START=1,REQUEST_STOP,REQUEST_DISCARD,REQUEST_ATTACH,REQUEST_CLOSE } CaptureRequestKind;
typedef enum { REQUEST_IDLE,REQUEST_QUEUED,REQUEST_EXECUTING,REQUEST_DONE } CaptureRequestState;
typedef struct {
 CaptureRequestKind kind;uint64_t session;PlCaptureConfig config;
 CaptureApplicationLimits limits;CaptureStorageRoute *route;
} CaptureRequestCommand;
typedef struct {
 uint64_t ticket;CaptureRequestKind kind;StorageResult result;
 uint64_t session;uint32_t config_id,source;
 CaptureApplicationState application_state;CaptureRuntimeState runtime_state;PlResult failure;
} CaptureRequestReceipt;
typedef struct {
 CaptureApplication *application;StorageQueueLocks locks;void *context;
 bool (*is_capture_owner)(void *),(*admit)(void *);
 uint64_t next_ticket,ticket,rejected;CaptureRequestState state;CaptureRequestCommand command;
 CaptureRequestReceipt receipt,pending;bool retain_receipt,pending_result;
} CaptureRequest;
/* One request and one retained execution receipt, sharing the control mutex.
 * submit STORE_OK means QUEUED. copy STORE_OK returns an executed receipt;
 * receipt.result means application accepted/rejected the request, never RUN,
 * STOPPED, durable storage, or hardware success. States are execution-time
 * observations only. Only exact ACK releases a retained receipt, so ACK START
 * before submitting STOP (STOP may cancel initialization still in progress).
 * No-lock configuration is for externally serialized Host tests only.
 * route lifetime extends through application use, not merely receipt ACK. */
bool capture_request_init(CaptureRequest *,CaptureApplication *,const StorageQueueLocks *,uint64_t,
                         bool (*owner)(void *),bool (*admit)(void *),void *);
/* retain=false is the compatibility path: executed result auto-releases;
 * callers must not await/ACK this ticket. Rejections remain counted. */
StorageResult capture_request_submit(CaptureRequest *,const CaptureRequestCommand *,bool retain,uint64_t *);
StorageResult capture_request_copy(CaptureRequest *,uint64_t,CaptureRequestReceipt *);
StorageResult capture_request_ack(CaptureRequest *,uint64_t);
/* Single capture owner only. Publication lock failure retains the applied
 * outcome locally; a later call publishes it without executing it again. */
void capture_request_step(CaptureRequest *);
#endif
