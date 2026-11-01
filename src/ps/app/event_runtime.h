#ifndef LFN_EVENT_RUNTIME_H
#define LFN_EVENT_RUNTIME_H
#include "summary_runtime.h"
#include "../capture/history.h"
#include "../events/event_engine.h"
#include "../events/event_feed.h"

typedef struct {
 void *context;
 StorageResult (*begin)(void *,uint64_t ordinal,uint64_t now_us);
 StorageResult (*status)(void *,uint64_t *file_id);
} EventRuntimeRollOps;
typedef struct {StorageEventMeta meta;bool used,dirty;uint64_t closed_us;} EventLateRecord;
typedef struct EventRuntime {
 EventEngine engine;EventFeed *feed;PcmHistory *history;SummaryRuntime *summary;
 StorageMultiObserver delegate;CaptureStorageRoute *route;StoragePump *pump;
 EventRuntimeRollOps roll;uint64_t session,generation,now_us,next_ordinal;
 StorageResult error;bool active,closing,stopping,roll_pending,roll_started,meta_open,missing;
 uint64_t segment_limit,confirmed_end,close_since,events_closed,segments_closed;
 EventRecord record;StorageEventMeta meta,submitted_meta;
 EventLateRecord late[EVENT_PENDING_SLOTS];int submitted_late;
 HistoryWatermark frozen;uint64_t prefetch_first,prefetch_end,prefetch_cursor;
 int32_t prefetch[HISTORY_SAMPLES],samples[HISTORY_COPY_MAX];
 uint8_t prefetch_valid[(HISTORY_SAMPLES+7)/8],valid[HISTORY_COPY_MAX/8];
 bool pending,pending_synced,submitting;StorageTicket ticket;RecordIdentity receipt;
 uint64_t pending_file;unsigned pending_kind;
} EventRuntime;
/* One storage owner, initialized once per SESSION. Capture owns history.
 * attach wraps the existing summary observer, never consumes C7 separately.
 * Optional summary=NULL is useful for explicit synthetic event-only fixtures. */
bool event_runtime_init(EventRuntime *,uint64_t,PcmHistory *,SummaryRuntime *,const EventRuntimeRollOps *);
/* Bind before attach, with producer stopped; persistence remains storage-owned. */
bool event_runtime_use_feed(EventRuntime *,EventFeed *);
bool event_runtime_attach(void *,StorageMultiSink *,CaptureStorageRoute *,StoragePump *const *,unsigned,uint64_t);
/* Called by EVENT roll before local definition admissions; no source reset. */
bool event_runtime_bind_file(EventRuntime *,StorageService *,uint64_t file_id);
/* Bounded history copy <=4096 samples, one direct offer, no backend IO. */
void event_runtime_step(EventRuntime *,uint64_t now_us);
#endif
