#ifndef LFN_ANALYSIS_SERVICE_H
#define LFN_ANALYSIS_SERVICE_H
#include "block_pool.h"
#include "../dsp/spectrum.h"

typedef struct {
    void *context;
    uint64_t (*monotonic_ns)(void *);
    /* Matching bounded platform critical sections / priority-inheriting
     * mutexes. Omit pairs only for a serialized single-thread consumer. */
    bool (*pool_lock)(void *);
    void (*pool_unlock)(void *);
    void (*snapshot_lock)(void *),(*snapshot_unlock)(void *);
} LfnAnalysisHooks;
typedef struct {
    uint64_t accepted,rejected_packet,rejected_identity,rejected_order,ignored_raw;
    uint64_t missing_frames,release_errors,pool_busy,max_core_ns,max_step_ns;
    unsigned queue_highwater;
} LfnAnalysisHealth;
typedef struct {
    uint64_t version,spectrum_at_ns,health_at_ns;
    bool has_spectrum;
    LfnAnalysisConfig config;
    LfnSpectrum spectrum;
    LfnAnalysisHealth health;
} LfnAnalysisSnapshot;
typedef struct {
    BlockPool *pool;
    uint64_t session_id,capture_id,last_seq;
    bool have_seq;
    bool pending_release;
    BlockHandle release_handle;
    LfnAnalysisConfig config;
    LfnAnalysisHooks hooks;
    LfnAnalysisHealth health;
    /* Working spectrum belongs solely to analysis; never exposed to clients.
     * Snapshot banks are copied only under snapshot lock. */
    LfnSpectrum working;
    LfnAnalysisSnapshot snapshots[2];
    unsigned active;
} LfnAnalysisService;
bool lfn_analysis_init(LfnAnalysisService *,BlockPool *,uint64_t session_id,
                        uint64_t capture_id,const LfnAnalysisConfig *,const LfnAnalysisHooks *);
/* Processes at most one complete queued block; true means a block consumed,
 * regardless of accept/reject. No allocation, IO or indefinite loop. */
bool lfn_analysis_step(LfnAnalysisService *);
/* Full private consumer copy, safe to format/send after unlocking. */
bool lfn_analysis_snapshot_copy(LfnAnalysisService *,LfnAnalysisSnapshot *destination);
/* Bounded full-PSD RAM export, JSON u64 strings and null invalid levels.
 * Formatting occurs only on caller's private copy. Failure clears output. */
bool lfn_analysis_snapshot_json(const LfnAnalysisSnapshot *,uint64_t now_ns,
                                  char *destination,size_t capacity,size_t *written);
#endif
