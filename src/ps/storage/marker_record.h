#ifndef LFN_MARKER_RECORD_H
#define LFN_MARKER_RECORD_H
#include "../app/marker.h"
#include "metadata_json.h"
#include "storage_queue.h"
/* MARKER JSON v1 is DATA. Point markers use equal header endpoints; range
 * endpoints retain the source sample stamp, not a guessed sample duration.
 * All 64-bit numbers are decimal strings. Decoder leaves parsed scratch for
 * the caller's local SESSION device check; it does not prove durability. */
bool storage_marker_record(const char *,const LfnMarker *,StorageRequest *,uint8_t *,size_t);
bool storage_marker_decode(const RecordIdentity *,const uint8_t *,size_t,MetadataJson *,LfnMarker *);
/* Decode a validated JSON string token into a LFN_MARKER_NOTE_BYTES+1 buffer. */
bool storage_marker_note_decode(const MetadataJson *,int,char *);
#endif
