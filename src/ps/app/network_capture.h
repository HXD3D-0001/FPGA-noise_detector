#ifndef LFN_NETWORK_CAPTURE_H
#define LFN_NETWORK_CAPTURE_H
#include "pre_adc_controller.h"
typedef struct {
 bool have_controller,storage_available,storage_active,storage_closed,storage_failed,storage_ready;
 uint64_t storage_session;PreAdcStatus controller;uint32_t source,channel;
} NetworkCaptureObservation;
typedef struct {
 void *context;
 bool (*can_begin)(void *,uint64_t);
 bool (*storage_begin)(void *,uint64_t);
 bool (*begin)(void *,uint64_t);
 bool (*start)(void *);
 bool (*stop)(void *);
} NetworkCaptureHooks;
typedef enum {NET_CAPTURE_IDLE,NET_CAPTURE_BEGIN,NET_CAPTURE_STORAGE,NET_CAPTURE_START,
 NET_CAPTURE_RUNNING,NET_CAPTURE_STOP,NET_CAPTURE_STOPPED,NET_CAPTURE_ERROR} NetworkCapturePhase;
typedef struct {
 NetworkCapturePhase phase;uint64_t session,since;int32_t failure;
 bool stop_requested,stop_sent,owns_start;NetworkCaptureHooks hooks;NetworkCaptureObservation observed;
} NetworkCapture;
void network_capture_init(NetworkCapture *,const NetworkCaptureHooks *);
/* All state APIs run on the capture owner. HTTP uses a separate copied mailbox. */
int network_capture_start(NetworkCapture *,uint64_t,uint64_t);
int network_capture_stop(NetworkCapture *,uint64_t);
void network_capture_step(NetworkCapture *,const NetworkCaptureObservation *,uint64_t);
bool network_capture_json(const NetworkCapture *,char *,size_t);
#endif
