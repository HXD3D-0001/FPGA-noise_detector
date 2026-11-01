#ifndef LFN_EVENT_META_H
#define LFN_EVENT_META_H
#include "metadata_json.h"
#include "storage_queue.h"
enum { STORAGE_EVENT_ITEMS=32 };
typedef enum { EVENT_META_OPEN,EVENT_META_APPEND,EVENT_META_CLOSED,EVENT_META_INCOMPLETE } StorageEventState;
typedef struct {uint64_t file_id,record_seq;} StorageEventLink;
typedef struct {uint64_t file_id,record_seq,first_raw,end_raw;uint32_t raw_stride,sample_count;} StorageEventPcmRef;
typedef struct {uint64_t first_raw,end_raw;} StorageEventGap;
typedef struct {
 char device_id[64];uint64_t session_id,capture_id;uint32_t config_id;
 uint64_t event_id,persist_revision;uint32_t segment_no;StorageEventLink prev_meta;
 StorageEventState state;bool event_closed;char close_reason[32];
 uint64_t onset_raw,prehistory_requested_first,actual_first,end_raw,append_cursor;
 uint64_t triggers[32],trigger_total;unsigned trigger_count;bool triggers_truncated;
 StorageEventPcmRef pcm[32];unsigned pcm_count;StorageEventGap gaps[32];unsigned gap_count;bool gaps_truncated;
 uint64_t valid_sample_count;bool prehistory_short,tail_missing;char missing_ref_reason[32];
 uint64_t evidence_count,evidence_first_raw,evidence_end_raw;char evidence_reason[32];
} StorageEventMeta;
/* Each immutable revision contains only NEW PCM refs/gaps and cumulative valid
 * sample count. persist_revision belongs to this segment's persistence chain,
 * independent of EventEngine revision. OPEN=1, later revisions link prev_meta.
 * CLOSED + event_closed=false + SEGMENT_LIMIT closes a physical segment only.
 * Late evidence must preserve CLOSED/range/append_cursor and add no PCM/gaps;
 * cross-revision proof belongs to runtime/reconstructor, not this local codec.
 * A valid codec result never proves a referenced PCM receipt was SYNCED. */
bool storage_event_decode(const RecordIdentity *,const uint8_t *,size_t,MetadataJson *,StorageEventMeta *);
bool storage_event_record(const StorageEventMeta *,StorageRequest *,uint8_t *,size_t);
#endif
