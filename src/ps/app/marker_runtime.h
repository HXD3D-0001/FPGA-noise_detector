#ifndef LFN_MARKER_RUNTIME_H
#define LFN_MARKER_RUNTIME_H
#include "marker.h"
#include "summary_runtime.h"
enum { MARKER_RUNTIME_QUEUE=4 };
typedef struct {
 uint64_t queued,synced,failed,unlinked,last_id;
 unsigned pending,error;
} MarkerRuntimeHealth;
typedef struct {
 atomic_flag lock;LfnMarker queue[MARKER_RUNTIME_QUEUE],current;
 unsigned head,count;bool accepting,closing,active,submitted,terminal_seen,submitting;
 uint64_t session,capture,started_us,now_us,file_id;uint32_t config;
 StorageResult error;StorageReceiptState terminal_state;StorageTicket ticket;RecordIdentity identity;
 SummaryRuntime *summary;StorageService *service;StorageServiceObserver service_delegate;
 StorageMultiObserver delegate;
 _Atomic uint64_t queued,synced,failed,last_id;
 _Atomic unsigned pending,health_error;
} MarkerRuntime;
/* Static lifetime, one capture producer and one storage owner. No allocation.
 * Enqueue tries one short lock and copies the marker, never waits or does IO. */
void marker_runtime_init(MarkerRuntime *,uint64_t,SummaryRuntime *);
void marker_runtime_enable(MarkerRuntime *,uint64_t capture,uint32_t config);
bool marker_runtime_enqueue(MarkerRuntime *,const LfnMarker *);
/* Install AFTER event/summary observers, once per epoch, before BEGIN. */
bool marker_runtime_attach(MarkerRuntime *,StorageMultiSink *,StorageService *summary);
void marker_runtime_step(MarkerRuntime *,uint64_t now_us);
void marker_runtime_health(const MarkerRuntime *,MarkerRuntimeHealth *);
#endif
