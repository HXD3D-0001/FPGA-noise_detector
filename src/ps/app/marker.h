#ifndef LFN_MARKER_H
#define LFN_MARKER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../time/time_map.h"

/* Tenant feeling marker: separate from algorithm events. Only the information
 * needed to compare/export is retained; callers must not put names/addresses
 * or other personal data into the note. */
enum { LFN_MARKER_CAPACITY=64, LFN_MARKER_NOTE_BYTES=512 };

typedef enum { LFN_MARKER_ADDED,LFN_MARKER_INVALID,LFN_MARKER_FULL } LfnMarkerResult;

typedef struct {
  LfnSampleStamp start,end;
  bool has_end;
  uint32_t strength; /* 1..5 */
  uint64_t submit_monotonic_ns;
  int64_t client_utc_ns;
  bool has_client_utc;
  uint64_t sync_map_id;
  uint64_t uncertainty_ns;
  const char *note;
  size_t note_bytes;
} LfnMarkerInput;

typedef struct {
  uint64_t marker_id;
  LfnSampleStamp start,end;
  bool has_end;
  uint32_t strength;
  uint64_t submit_monotonic_ns;
  int64_t client_utc_ns;
  bool has_client_utc;
  uint64_t sync_map_id;
  uint64_t uncertainty_ns;
  char note[LFN_MARKER_NOTE_BYTES+1];
} LfnMarker;

typedef struct {
  LfnMarker items[LFN_MARKER_CAPACITY];
  unsigned count;
  uint64_t next_id;
  uint64_t rejected;
} LfnMarkerStore;

void lfn_marker_store_init(LfnMarkerStore *);
LfnMarkerResult lfn_marker_add(LfnMarkerStore *,const LfnMarkerInput *);
const LfnMarker *lfn_marker_find(const LfnMarkerStore *,uint64_t marker_id);
/* Printable UTF-8 only; rejects malformed input and ASCII/C1 controls. */
bool lfn_marker_note_valid(const char *,size_t);
#endif
