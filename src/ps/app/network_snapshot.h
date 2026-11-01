#ifndef LFN_NETWORK_SNAPSHOT_H
#define LFN_NETWORK_SNAPSHOT_H
#include <stdbool.h>
#include <stddef.h>
#include "marker.h"
/* Single network task only: private static copies, no filesystem IO. Failure
 * clears output; the caller must not send an incomplete JSON response. */
bool lfn_network_image_json(const char *path,char *out,size_t capacity);
/* Implemented by the image owner with a short guarded copy. */
bool lfn_capture_image_markers_copy(LfnMarkerStore *);
#endif
