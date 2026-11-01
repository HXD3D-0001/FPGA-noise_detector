#ifndef LFN_TIME_SERVICE_H
#define LFN_TIME_SERVICE_H
#include "time_map.h"
#include "../storage/metadata_builder.h"
#include "../storage/storage_service.h"
typedef struct {
 StorageService *storage;uint64_t session_id;StorageMetadataBuilder builder;uint8_t payload[2048];
 StorageTicket pending_ticket;RecordIdentity pending_identity;LfnTimeMapEntry pending_map;
 LfnTimeMap active;LfnMonotonicCoverage coverage;bool pending,pending_synchronized,synchronized,failed;
} LfnTimeService;
/* Sole storage/control owner. Tick denominator uses monotonic time only.
 * TIME_MAP must be submitted/synced before activation; failures keep explicit
 * unavailable mapping. Earlier mappings remain immutable on disk; this cache
 * serves current map only, older ranges require historical metadata replay. */
bool lfn_time_service_init(LfnTimeService *,StorageService *,uint64_t session);
StorageSubmit lfn_time_service_submit(LfnTimeService *,const StorageTimeFacts *,double fs_uncertainty_ppm);
void lfn_time_service_poll(LfnTimeService *); /* queue poll only, no backend IO */
bool lfn_time_service_tick(LfnTimeService *,uint64_t monotonic_ns,bool requested,bool available);
bool lfn_time_service_utc(const LfnTimeService *,LfnSampleStamp,LfnUtcEstimate *);
bool lfn_time_record_map(const RecordIdentity *,const uint8_t *,size_t,MetadataJson *,LfnTimeMapEntry *,bool *synchronized);
#endif
