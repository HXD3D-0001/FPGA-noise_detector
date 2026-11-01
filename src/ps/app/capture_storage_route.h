#ifndef LFN_CAPTURE_STORAGE_ROUTE_H
#define LFN_CAPTURE_STORAGE_ROUTE_H
#include "../storage/storage_pump.h"
#include "capture_time_pair.h"
#include <stdatomic.h>
enum { CAPTURE_ROUTE_SLOTS=4,CAPTURE_ROUTE_LOSSES=8 };
typedef struct {
 void *context;StorageResult (*energy)(void *,const LfnServiceMessage *,StoragePumpTicket *);
 StorageResult (*psd)(void *,const LfnSpectrum *,StoragePumpTicket *);
 StorageResult (*pcm)(void *,uint64_t,const uint8_t *,size_t,StoragePumpTicket *);
 StorageResult (*loss)(void *,const StorageIngressLoss *,StoragePumpTicket *);
 StorageResult (*poll)(void *,StoragePumpTicket,StoragePumpResult *);
 StorageResult (*ack)(void *,StoragePumpTicket);StorageResult (*close)(void *);
} CaptureStorageSink;
typedef struct {uint64_t offered,copied,dropped,overflow,invalid;bool exhausted;} CaptureRouteSourceHealth;
typedef struct {uint64_t accepted,loss_accepted,retries,rejected;bool blocked,handoff_drained;StorageResult error;unsigned error_kind;} CaptureRouteConsumerHealth;
typedef struct {CaptureRouteSourceHealth source;CaptureRouteConsumerHealth consumer;bool attached,producer_closed;uint64_t generation;} CaptureRouteHealth;
typedef struct {
 uint64_t version,observed_us;LfnServiceIdentity identity;uint8_t bound_mask;
 uint64_t first_seq[4],first_raw[4];bool capture_valid,active_valid,started_valid,software_drained;
 PlCaptureSnapshot capture,started;PlContractSnapshot active;
 CaptureTimePair time_pair; /* immutable proof, distinct from publish observed_us */
 uint64_t generation,final_latest_raw;uint32_t final_capture_seq,final_drain_seq;
 bool final_clean;
} CaptureRouteFacts;
typedef struct {unsigned state;uint64_t order;LfnServiceIdentity identity;size_t bytes;unsigned kind;StoragePumpPayload payload;} CaptureRouteSlot;
typedef struct CaptureStorageRoute {
 uint64_t session_id;StorageQueueLocks locks;CaptureStorageSink sink;LfnServiceBridge *bridge;
 atomic_bool attached;CaptureRouteSlot slots[CAPTURE_ROUTE_SLOTS];CaptureRouteHealth published;CaptureRouteFacts facts;
 /* Single capture producer owns these fields even if try-lock fails. */
 CaptureRouteSourceHealth producer;uint64_t next_order,facts_version,loss_overflow;
 StorageIngressLoss losses[CAPTURE_ROUTE_LOSSES];unsigned loss_head,loss_count;
 bool have_psd;uint64_t psd_session,psd_capture,psd_seq;uint32_t psd_config;
 /* Single storage consumer owns these; no DMA pointers survive. */
 CaptureRouteConsumerHealth consumer;bool energy_pending,has_ticket,result_ready,prefer_energy;
 LfnServiceMessage energy;StoragePumpTicket ticket;StoragePumpResult result;unsigned active;
} CaptureStorageRoute;
bool capture_storage_route_init(CaptureStorageRoute *,uint64_t session,const StorageQueueLocks *,const CaptureStorageSink *);
/* Capture owner before first request; claims the C7 queue for the route's
 * entire lifetime. Generic/foreign consumers then receive BUSY. */
bool capture_storage_route_bind(CaptureStorageRoute *,LfnServiceBridge *);
/* Capture owner, before first request. BUSY preserves bridge lock/foreign
 * lifetime claim; no stealing, release or unsynchronized claim inspection. */
StorageResult capture_storage_route_bind_result(CaptureStorageRoute *,LfnServiceBridge *);
/* Both owners stopped, backend closed/unmounted by the storage owner. Releases
 * only an exact-generation, closed/drained route with no pending handoff. */
StorageResult capture_storage_route_unbind(CaptureStorageRoute *,uint64_t expected_generation);
/* Capture owner only. View must be from packet_decode, borrowed synchronously.
 * Copies only, no file IO/CRC/formatting/indefinite lock wait. */
bool capture_storage_route_packet(CaptureStorageRoute *,const LfnServiceIdentity *,const PacketView *);
bool capture_storage_route_psd(CaptureStorageRoute *,const LfnServiceIdentity *,const LfnSpectrum *);
void capture_storage_route_flush(CaptureStorageRoute *);
bool capture_storage_route_producer_close(CaptureStorageRoute *);
bool capture_storage_route_facts_publish(CaptureStorageRoute *,const CaptureRouteFacts *);
bool capture_storage_route_facts_copy(CaptureStorageRoute *,CaptureRouteFacts *);
bool capture_storage_route_health(CaptureStorageRoute *,CaptureRouteHealth *);
/* Dedicated storage task only; <=one offer or poll/ACK transaction per call.
 * No source/DMA ownership. Missing definitions retain data for explicit setup;
 * invalid sink results block visibly rather than silently discard. */
void capture_storage_route_step(CaptureStorageRoute *);
/* Both owners externally paused, old epoch physically closed, new backend
 * definitions ready. No concurrent producer/consumer calls, no IO. Preserve C7
 * lifetime claim, order/loss counters and PSD dedup. Exact generation CAS-like
 * check; success increments generation, invalidates old facts, opens ingress.
 * Platform controller must enforce old/new epoch readiness before calling. */
StorageResult capture_storage_route_reconnect(CaptureStorageRoute *,const CaptureStorageSink *,uint64_t expected_generation);
#endif
