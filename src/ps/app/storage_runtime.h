#ifndef LFN_STORAGE_RUNTIME_H
#define LFN_STORAGE_RUNTIME_H
#include "storage_owner_config.h"
#include <stdint.h>
#include <stdbool.h>
#include "../drivers/capture_regs.h"
#include "../capture/history.h"
#include "marker_runtime.h"
/* Production capture->SD storage coordinator. One task owns the mounted card,
 * the multi-file owner and the capture route consumer; the capture owner feeds
 * the same route through capture_application_attach_storage. The coordinator is
 * started at boot and waits for host-session commands from the UART/application
 * layer. Rotation creates fresh epoch directories and uses the capture-owner
 * STOP/PAUSE/RESUME mailbox; a new host session creates a NEW directory. */
typedef struct {
 uint64_t bytes,accepted,synced,invalid,unsaved_overflow;
 unsigned live_jobs,unsaved;
 bool service_fatal,pump_fatal,queue_valid;
} StorageRuntimeFileHealth;
typedef struct {
 uint64_t step,dropped,overflow,accepted;
 unsigned error_kind,error;
 bool valid,blocked;
 StorageRuntimeFileHealth files[STORAGE_OWNER_FILES];
 uint64_t summary_count,summary_capture_revision;unsigned summary_pending,summary_error;
 MarkerRuntimeHealth markers;
} StorageRuntimeDiagnostics;
typedef struct {
 bool started,active,closed,failed;
 uint64_t session,steps;
 unsigned state,phase,error;
 uint32_t sd_stage;
 uint32_t rotations,epoch;
 uint64_t route_generation;
 bool restart_pending;
 uint32_t rotation_stage,rotation_error;
 StorageRuntimeDiagnostics diagnostics;
} StorageRuntimeHealth;
bool lfn_storage_runtime_start(void);
/* Set once at boot before the capture producer starts. */
bool lfn_storage_runtime_observe(LfnServiceBridge *);
/* Initialized once before capture starts; capture writes, storage reads. */
PcmHistory *lfn_storage_runtime_history(void);
/* True means copied into the bounded mailbox, not saved. Diagnostics expose
 * the number of exact SYNCED markers and any failed submissions. */
bool lfn_storage_runtime_marker(const LfnMarker *);
/* Called after a successful BEGIN: start one storage session for this host
 * session token. Returns false if a session is already active/queued. */
bool lfn_storage_runtime_session(uint64_t host_session);
/* Called on host STOP: ask the capture owner to close its producer and drain
 * the storage epoch. Idempotent. */
void lfn_storage_runtime_stop(void);
/* Normal user STOP first reserves a stable epoch. Retry until true before
 * stopping capture: an in-flight rotation finishes its new metadata first.
 * The owner then suppresses further rotations until this session closes.
 * Failed/inactive storage does not delay the existing capture cleanup. */
bool lfn_storage_runtime_prepare_stop(void);
bool lfn_storage_runtime_capture_close_requested(void);
/* Force one owner rotation for the ACTIVE session (board test hook; the task
 * also rotates automatically on max_file_bytes/max_file_age_us). */
bool lfn_storage_runtime_request_rotation(void);
/* Operational opt-in for automatic rotation (age/bytes); default off so
 * identity-stable M1 captures remain unchanged. */
bool lfn_storage_runtime_auto_rotate(bool enable);
/* True once the cold epoch is attached and staged, i.e. a host START is safe
 * (the capture owner may attach the route only before START). */
bool lfn_storage_runtime_ready_for_start(void);
/* Latched until reboot: capture owner must use its normal fault cleanup path.
 * Failed storage retains evidence; it is never a clean, reusable session. */
bool lfn_storage_runtime_failed(void);
bool lfn_storage_runtime_health(StorageRuntimeHealth *);
/* Capture owner publishes actual source/software quiescence each iteration. */
void lfn_storage_runtime_capture_stopped(bool stopped);
/* Capture owner consumes this only after rotation's RESUME ACK. It must issue
 * a normal START with a fresh capture identity; no synthetic facts/ACKs. */
bool lfn_storage_runtime_take_restart(PlCaptureConfig *config);
/* Set before BEGIN. Bounds are per epoch; policy persists across sessions. */
bool lfn_storage_runtime_limits(uint64_t max_file_bytes,uint64_t max_file_age_us);
#endif
