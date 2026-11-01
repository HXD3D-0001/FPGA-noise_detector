#ifndef LFN_STORAGE_SEGMENT_H
#define LFN_STORAGE_SEGMENT_H
#include "capture_storage_route.h"
#include "../storage/metadata_builder.h"
#include "../storage/storage_bootstrap.h"
typedef enum { SEGMENT_IDLE,SEGMENT_SUBMIT,SEGMENT_WAIT,SEGMENT_SYNCED,SEGMENT_FAILED } StorageSegmentState;
typedef struct {
 StorageService *services[STORAGE_FAMILY_FILES];unsigned count,file,record;
 void *owner_context;bool (*is_owner)(void *);uint64_t session_id,next_request,request_id,facts_version;
 StorageSegmentState state;StorageResult error;StorageTicket ticket;
 StorageMetadataBuilder builder;StorageDefinitionCopy definitions[2];
} StorageSegment;
/* Persistent private workspace, sole storage executor. All target SESSION
 * definitions already observed SYNCED. No IO in init/begin/step/ack.
 * External task transport must copy/own input facts; no live capture reads. */
bool storage_segment_init(StorageSegment *,StorageService *const *,unsigned,uint64_t session,uint64_t next_request,bool (*owner)(void *),void *);
/* cfg/clock supplied explicitly; START/ACTIVE copied ONLY from the route facts.
 * Require valid flags/session/config/capture and OPEN revision1. Clock fields
 * are actual caller time-map facts, never guessed from nominal/first packet.
 * Build both JSONs immediately; no input strings/pointers retained afterward.
 * token assigned only on successful admission. Exact duplicate definitions
 * can reuse only byte-identical payload + retained observed-SYNCED receipt. */
StorageResult storage_segment_begin(StorageSegment *,const CaptureRouteFacts *,const StorageConfigFacts *,const StorageCaptureFacts *clock,uint64_t *request_token);
void storage_segment_step(StorageSegment *);
/* Exact token ACK after all files SYNCED; failure is quarantined, no rollback,
 * definition release, implicit reinit or retry. Tickets/payload remain visible. */
StorageResult storage_segment_ack(StorageSegment *,uint64_t);
#endif
