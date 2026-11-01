#ifndef LFN_STORAGE_MULTI_SINK_H
#define LFN_STORAGE_MULTI_SINK_H
#include "capture_storage_route.h"
#include "../storage/storage_family.h"
#define LFN_STORAGE_OBSERVERS 1
typedef struct {
 StoragePumpKind kind;uint64_t session;size_t bytes;
 union {const LfnServiceMessage *energy;const LfnSpectrum *psd;const uint8_t *pcm;const StorageIngressLoss *loss;} value;
} StorageMultiSource;
typedef struct {
 void *context;
 StorageResult (*preflight)(void *,const StorageMultiSource *);
 bool (*commit)(void *,const StorageMultiSource *,StoragePump *,StoragePumpTicket);
 void (*cancel)(void *);
 StorageResult (*before_close)(void *);
} StorageMultiObserver;
typedef struct {StoragePump *energy,*psd,*pcm,*raw;} StorageMultiTargets;
typedef struct {
 uint64_t session_id,next_ticket;StorageMultiTargets targets;
 StoragePump *active_pump;StoragePumpTicket underlying,public_ticket;
 StoragePump *close_pumps[STORAGE_FAMILY_FILES];unsigned close_count;
 bool active,closing;unsigned close_cursor;
 StorageMultiObserver observer;bool fatal;
} StorageMultiSink;
typedef bool (*StorageMultiAttach)(void *,StorageMultiSink *,CaptureStorageRoute *,StoragePump *const *,unsigned,uint64_t);
/* Sole storage consumer adapter; no IO, locks or allocation of its own.
 * Existing CaptureStorageRoute already bounds/copies ingress and owns one
 * outstanding command. Four explicit destinations may alias intentionally.
 * Each destination requires own local definitions and same session. Factory
 * READY alone does NOT prove definitions synced. Poll/ACK retain exact pump.
 * next_ticket must never reuse this adapter's old generations in a session.
 * Reconnect only after old route/pump handoff and no outstanding old tickets. */
bool storage_multi_sink_init(StorageMultiSink *,uint64_t session,uint64_t next_ticket,
 const StorageMultiTargets *,CaptureStorageSink *);
/* Idle, non-closing sink only. Distinct same-session pumps must include every
 * source destination, plus all derived-file pumps owned by this epoch. */
bool storage_multi_sink_set_close_pumps(StorageMultiSink *,StoragePump *const *,unsigned count);
/* Pure vtable identity check, not a metadata/SYNCED readiness proof. */
bool storage_multi_sink_matches(StorageMultiSink *,const CaptureStorageSink *);
/* Existing initialized object only; no reset/ticket seed change. */
bool storage_multi_sink_view(StorageMultiSink *,CaptureStorageSink *);
#endif
