#ifndef LFN_STORAGE_TASK_FREERTOS_H
#define LFN_STORAGE_TASK_FREERTOS_H
#include "storage_pump.h"
#include "metadata_builder.h"
#include "../app/capture_storage_route.h"
#include "fatfs_backend.h"
#include "../app/storage_owner_freertos.h"
enum { STORAGE_TASK_STACK_WORDS=6144,STORAGE_TASK_HEAP_MARGIN=8192 };
typedef struct {
 _Alignas(64) uint8_t arena[STORAGE_POOL_BYTES];
 StorageMetadataRegistry registry;StorageWriter writer;StorageQueue queue;
 StorageService service;StorageMetadataBuilder builder;StoragePump pump;CaptureStorageRoute route;
 /* Task-owned FatFs mount and NEW file handle; unused by explicit-backend mode. */
 LfnFatFsVolume volume;LfnFatFsFile file;char relative_path[LFN_FATFS_PATH];
#ifdef LFN_STORAGE_OWNER_INSTANCE
 /* P4 multi-file owner instance: full 2x(2 data+ledger+catalog) workspace.
  * Only the storage-task build defines this macro; probe images stay small.
  * Lives only in this task's NOLOAD resource block; no malloc. */
 StorageOwner owner;
 StorageOwnerFreertos owner_fs;
#endif
} StorageTaskResources;
typedef struct {bool started,ready,startup_failed;bool sd_mode,mounted,closed,close_failed;StorageIoResult sd_fault;uint32_t sd_stage;unsigned sd_last_error,loop_steps,loop_stage,pump_stage;
 bool service_pending,sealed,drained,fatal;int service_pending_result;uint64_t service_accepted;
 uint64_t recovered_records,file_prefix,file_bytes;bool tail_repaired;int tail_reason;
 unsigned pre_writer_state,pre_writer_error,pre_tail_reason;StoragePumpHealth pump;CaptureRouteHealth route;uint32_t stack_unused_words;size_t free_heap;} StorageTaskHealth;
/* Task context AFTER scheduler starts. Explicit I/O backend, new empty file,
 * caller-established identity and seed. No mount/DDR init/download/auto START.
 * priority1..5 stays below existing capture priority6. Start only ONCE. */
bool lfn_storage_task_start(const StorageBackend *,uint64_t session,uint64_t file,uint64_t next_sequence,uint64_t sync_us,unsigned priority);
/* Task-context SD startup: the storage task itself mounts logical volume "0:"
 * and creates the NEW 8.3 file. No format, overwrite, mkdir or boot media.
 * Close waits until the sealed service is fully drained, then f_sync+f_close
 * and unmount run in the same task before sd_closed is published. */
bool lfn_storage_task_start_sd(uint64_t session,uint64_t file,uint64_t next_sequence,uint64_t sync_us,unsigned priority,const char *relative_path);
/* Existing-file recovery variant: opens the named file WITHOUT create, scans
 * the valid same-session/strict-sequence prefix, repairs a partial tail and
 * publishes the recovered counters. Never truncates a valid prefix. */
bool lfn_storage_task_start_sd_existing(uint64_t session,uint64_t file,uint64_t next_sequence,uint64_t sync_us,unsigned priority,const char *relative_path);
bool lfn_storage_task_connect_capture(void);
bool lfn_storage_task_source_facts(CaptureRouteFacts *);
StorageResult lfn_storage_task_metadata(const RecordIdentity *,const uint8_t *,size_t,StoragePumpTicket *);
StorageResult lfn_storage_task_energy(const LfnServiceMessage *,StoragePumpTicket *);
StorageResult lfn_storage_task_summary(const MinuteSummary *,StoragePumpTicket *);
StorageResult lfn_storage_task_psd(const LfnSpectrum *,StoragePumpTicket *);
StorageResult lfn_storage_task_pcm(uint64_t,const uint8_t *,size_t,StoragePumpTicket *);
StorageResult lfn_storage_task_poll(StoragePumpTicket,StoragePumpResult *);
StorageResult lfn_storage_task_ack(StoragePumpTicket);
StorageResult lfn_storage_task_abort(StoragePumpTicket);
StorageResult lfn_storage_task_close(void);
StorageResult lfn_storage_task_recover(bool repair_tail);
bool lfn_storage_task_health(StorageTaskHealth *);
/* True only after the in-task f_sync+f_close+unmount completed successfully. */
bool lfn_storage_task_sd_closed(void);
#ifdef LFN_STORAGE_OWNER_INSTANCE
/* Probe/development multi-file mode: the task mounts SD0 itself, binds the
 * namespace guard and drives the real StorageOwner through provision under an
 * EXISTING caller-created root directory. Capture ports are stubs so the owner
 * stops at READY (no cold start/rotation in this mode yet); the probe then
 * resets the board and inspects the created files with a read-only scan. */
bool lfn_storage_task_start_sd_multifile(uint64_t session,uint64_t seed,uint64_t sync_us,unsigned priority,const char *root);
/* Crash-consistency probe: cold start then continuously offer PSD records until
 * the board is reset; no rotation/stop is attempted by the task itself. */
bool lfn_storage_task_start_sd_write(uint64_t session,uint64_t seed,uint64_t sync_us,unsigned priority,const char *root,unsigned target);
typedef struct {bool started,mode,ready,failed,closed;unsigned owner_state,owner_error,phase;uint64_t steps,written;unsigned write_target;uint32_t sd_stage;unsigned last_error,epoch_state,lifecycle_state,rotation_state,route_attached;uint64_t route_generation;StorageIoResult sd_fault;} StorageTaskOwnerHealth;
bool lfn_storage_task_owner_health(StorageTaskOwnerHealth *);
#endif
#endif