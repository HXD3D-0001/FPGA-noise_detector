#include "storage_rotation.h"
#include "capture_image.h"
#include "storage_cold_start.h"
static StorageResult submit(void *ctx,CaptureControlKind k,uint64_t pause,uint64_t generation,uint64_t *ticket){(void)ctx;return lfn_capture_image_control_submit(k,pause,generation,ticket);}
static StorageResult copy(void *ctx,uint64_t ticket,CaptureControlReceipt *out){(void)ctx;return lfn_capture_image_control_copy(ticket,out);}
static StorageResult ack(void *ctx,uint64_t ticket){(void)ctx;return lfn_capture_image_control_ack(ticket);}
bool lfn_capture_image_rotation_port(StorageRotationCapturePort *out){
 if(!out)return false;
 *out=(StorageRotationCapturePort){NULL,submit,copy,ack};return true;
}

static StorageResult attach(void *ctx,CaptureStorageRoute *r,uint64_t *ticket){(void)ctx;return lfn_capture_image_control_attach(r,ticket);}
bool lfn_capture_image_cold_port(StorageColdCapturePort *out){
 if(!out)return false;
 *out=(StorageColdCapturePort){NULL,attach,copy,ack};return true;
}
