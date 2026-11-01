#ifndef LFN_STORAGE_OWNER_H
#define LFN_STORAGE_OWNER_H
#include "storage_owner_config.h"
#include "storage_cold_start.h"
#include "storage_rotation.h"
#include "storage_segment.h"
#include "../storage/storage_engine.h"
#include "../storage/storage_event_roll.h"
#include "../storage/storage_epoch.h"
#include "../storage/storage_factory.h"
#include "../storage/storage_family.h"
#include "../storage/storage_family_queue.h"
#include "../storage/storage_file_catalog.h"
#include "../storage/storage_file_handles.h"
#include "../storage/storage_lifecycle.h"
#include "../storage/storage_namespace.h"
#include "../storage/storage_provision.h"
#include "../storage/storage_queue.h"
#include <stddef.h>
/* Production multi-file storage owner: assembles the provision -> factory ->
 * lifecycle -> capture epoch -> segment -> multi sink -> rotation stack for
 * exactly two alternating persistent groups (2 or 4 data + ledger + catalog)
 * plus one shared capture route. It owns no new storage state machine; every
 * transition is delegated to the existing single-step modules.
 *
 * Ownership/lifetime:
 * - One static StorageOwner per storage executor; no malloc, no recursion.
 *   The caller supplies the already-mounted namespace adapter (StorageFileOps
 *   from lfn_fatfs_file_port_init/lfn_fatfs_namespace_port_bind), the NEW-file
 *   provision adapter (StorageProvisionOps from lfn_fatfs_provision_port_init)
 *   and the capture-owner mailbox adapters (StorageColdCapturePort,
 *   StorageRotationCapturePort from lfn_capture_image_cold_port /
 *   lfn_capture_image_rotation_port). FatFs headers are intentionally not
 *   included here so Host builds stay filesystem-free; the future FreeRTOS
 *   wrapper owns those adapter objects.
 * - init is pure: it registers both groups' handle specs, arms the namespace
 *   guard, wires the capture epoch hooks, the factory/provision geometry and
 *   the rotation executor. It performs no IO, mount, create or START. The sink,
 *   route and segment are assembled inside step once the factory is READY
 *   (they need initialized service queues), still without backend IO.
 * - Callers must provide an actual producer-stopped proof at init (source off,
 *   software drained, no producer calls in flight); the modules refuse to
 *   assemble otherwise. The owner never fabricates that proof.
 * - Only the storage executor may call the step and begin/request APIs.
 *   Producers reach data through CaptureStorageRoute (storage_owner_route) and
 *   the capture-owner command mailboxes, never through these objects directly.
 * - Failures are latched: the owner stops stepping and keeps every ticket,
 *   lease, opened handle and definition prefix for explicit recovery. Nothing
 *   is closed, deleted, retried or rolled back automatically.
 * - The two groups alternate after each rotation; the next group's handles are
 *   discarded and re-registered from the caller's next spec so a real platform
 *   can supply fresh IDs/paths per epoch. The adapter must still create NEW
 *   EMPTY files; an existing path must be refused, never overwritten. */
enum {
 STORAGE_OWNER_NAMES=16,
 STORAGE_OWNER_NAME=13,
 STORAGE_OWNER_TARGETS=4
};
typedef struct {
 uint64_t file_id,catalog_id,ledger_id,order_key;
 StorageCatalogRole role;StorageRetentionFamily family;
 char path[STORAGE_HANDLE_PATH];
 char name[STORAGE_CATALOG_NAME];
 StorageCatalogLocation location;uint64_t ordinal;
} StorageOwnerFileSpec;
typedef struct {
 StorageOwnerFileSpec data[STORAGE_OWNER_FILES];
 StorageOwnerFileSpec ledger,catalog;
 /* Group 0 must leave both zero: cold start derives CONFIG/CAPTURE from the
  * actual START/ACTIVE facts. Later groups may leave both zero (inherit the
  * sealed epoch's identity) or assert the exact expected values. */
 uint32_t config_id;uint64_t capture_id;
} StorageOwnerGroupSpec;
typedef struct {char name[STORAGE_OWNER_NAME];bool snapshot_metadata;} StorageOwnerNamespaceName;
/* Filesystem/provision/capture injection points. files and provision normally
 * come from the FatFs port adapters; cold/rotation are the capture-owner
 * mailbox ports. is_owner/producer_stopped are the storage-task and capture
 * source-off proofs, never constants on target. */
typedef struct {
 StorageFileOps files;
 StorageProvisionOps provision;
 StorageColdCapturePort cold;
 StorageRotationCapturePort rotation;
 bool (*is_owner)(void *);
 void *owner_context;
 bool (*producer_stopped)(void *);
 void *proof_context;
} StorageOwnerPorts;
typedef struct {
 uint64_t session_id;
 uint64_t next_sequence;
 uint64_t segment_seed;
 uint64_t multi_seed;
 uint64_t sync_us,max_file_bytes,max_file_age_us,created_us;
 /* Platform capture-owner service budget; the storage owner performs no
  * blocking wait and only requires the caller to declare it explicitly. */
 uint64_t owner_us;
 const char *root;
 const StorageOwnerNamespaceName *names;
 unsigned name_count;
 unsigned targets[STORAGE_OWNER_TARGETS];
 StorageOwnerGroupSpec groups[STORAGE_OWNER_GROUPS];
 StorageOwnerPorts ports;
 StorageQueueLocks locks;
 StorageQueueLocks route_locks;
 StorageMultiAttach attach_observers;void *observer_context;
} StorageOwnerConfig;
/* Fresh layout for the epoch that replaces the current one at rotation. */
typedef struct {
 uint64_t segment_seed;
 uint64_t sync_us,max_file_bytes,max_file_age_us;
 bool replace_layout;
 StorageOwnerFileSpec files[STORAGE_OWNER_FILES];
 StorageOwnerFileSpec ledger,catalog;
} StorageOwnerNextSpec;
typedef struct {
 _Alignas(64) uint8_t arena[STORAGE_POOL_BYTES];
 StorageQueuePool pool;
 StorageFamily family;
 StorageFamilyQueue family_queue;
 StorageFileCatalog catalog;
 StorageFileVerifier verifier;
 StorageEpoch epoch;
 StorageEngine engine;
 StorageBinding bindings[STORAGE_OWNER_FILES];
 StorageWriter writers[STORAGE_OWNER_FILES];
 StorageQueue queues[STORAGE_OWNER_FILES];
 StorageService services[STORAGE_OWNER_FILES];
 StoragePump pumps[STORAGE_OWNER_FILES];
 StorageBootstrapPlan plans[STORAGE_OWNER_FILES];
 StorageProvision provision;
 StorageFactory factory;
 StorageLifecycle lifecycle;
 StorageCaptureEpoch capture_epoch;
 StorageMultiSink sink;
 StorageSegment segment;
 unsigned slots[STORAGE_OWNER_GROUP_FILES];
} StorageOwnerGroup;
typedef struct {
 StorageFileHandles handles;
 StorageNamespace space;
 CaptureStorageRoute route;
 StorageOwnerGroup groups[STORAGE_OWNER_GROUPS];
} StorageOwnerWorkspace;
typedef enum {
 OWNER_PREPARED,OWNER_PROVISION,OWNER_READY,OWNER_COLD,OWNER_WAIT_FACTS,OWNER_RUNNING,
 OWNER_STOPPING,OWNER_STOPPED,OWNER_ROTATING,OWNER_FAILED
} StorageOwnerState;
typedef struct {
 StorageOwnerWorkspace workspace;
 StorageColdStart cold;
 StorageRotation rotation;
 /* Persistent executor request filled from next; no large stack staging. */
 StorageRotationNext rotation_next;
 StorageOwnerConfig config;
 char namespace_root[192];
 StorageOwnerNamespaceName namespace_names[STORAGE_OWNER_NAMES];
 unsigned name_count;
 StorageOwnerNextSpec next;
 bool have_next;
 unsigned group;
 bool have_time;
 uint64_t last_now;
 StorageOwnerState state,failed_at;
 StorageResult error;
 StorageEventRoll event_roll;
} StorageOwner;
/* Pure assembly. Requires both groups' handles to fit STORAGE_HANDLES and the
 * namespace allowlist to cover data/ledger (ordinary) and catalog (metadata
 * snapshot) basenames. Rejected init leaves no ownership claim; the owner may
 * be re-initialized with a corrected config. */
bool storage_owner_init(StorageOwner *,const StorageOwnerConfig *);
/* Two-phase target assembly. prepare() is pure (namespace initialized, all
 * geometry/IDs validated) but does NOT require the real file/provision adapters
 * yet; attach_ports() installs the final adapters and completes the same
 * assembly as init(). This lets a mounted-volume wrapper bind the namespace
 * guard before mount while still providing file/provision ops afterwards. */
bool storage_owner_prepare(StorageOwner *,const StorageOwnerConfig *);
/* Diagnostic: last failed next-layout check code (0 when the last check passed). */
extern uint32_t storage_owner_next_trace;
extern uint32_t storage_owner_attach_trace;
bool storage_owner_attach_ports(StorageOwner *,const StorageOwnerPorts *);
/* OWNER_READY only. Copies the SESSION facts into the cold controller and arms
 * the gated cold path; no IO and no START. */
StorageResult storage_owner_begin_cold(StorageOwner *,const StorageSessionFacts *);
/* OWNER_WAIT_FACTS only. Explicit platform CONFIG/time facts for the actual
 * START; never inferred from nominal rates or first received data. */
StorageResult storage_owner_segment(StorageOwner *,const StorageConfigFacts *,const StorageCaptureFacts *);
/* <= one abstract backend operation per call. Advances exactly one owner
 * phase: provision, cold, running epoch consumption, stop drain/close. */
void storage_owner_step(StorageOwner *,uint64_t monotonic_us);
/* Delegate normal STOP to the capture epoch. Caller already stopped/flushed
 * hardware and closed the route producer. The owner then drains/closes the
 * epoch and ends OWNER_STOPPED; there is no restart or auto-rotation. */
StorageResult storage_owner_request_stop(StorageOwner *);
/* OWNER_RUNNING only. Starts one rotation into the other persistent group and
 * stores the next spec; the capture-owner STOP_CLOSE/PAUSE/RESUME handshake is
 * driven by storage_rotation_step. As soon as the old epoch seals, the owner
 * fills its persistent StorageRotationNext from the alternate group and calls
 * storage_rotation_prepare. OK means the rotation executor started, not that
 * the new epoch is active; step until OWNER_RUNNING again. */
StorageResult storage_owner_begin_rotation(StorageOwner *,const StorageOwnerNextSpec *);
/* EVENT producer must have stopped admission at the segment boundary. Other
 * capture streams keep running. ordinal is consumed by the session runtime. */
StorageResult storage_owner_begin_event_roll(StorageOwner *,const StorageEventRollOps *,uint64_t ordinal,uint64_t created_us);
StorageOwnerState storage_owner_state(const StorageOwner *);
StorageResult storage_owner_error(const StorageOwner *);
/* Shared storage-side route handed to the capture owner through the cold
 * attach mailbox; producers feed it, the storage owner consumes it. */
CaptureStorageRoute *storage_owner_route(StorageOwner *);
#endif
