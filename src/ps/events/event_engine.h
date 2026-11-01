#ifndef LFN_EVENT_ENGINE_H
#define LFN_EVENT_ENGINE_H
#include "../app/service_bridge.h"
enum { EVENT_BACKGROUND_SLOTS=470, EVENT_TRIGGER_SLOTS=32, EVENT_PENDING_SLOTS=16, EVENT_OUTPUT_SLOTS=32 };
typedef enum { EVENT_WARMUP, EVENT_IDLE, EVENT_CANDIDATE, EVENT_ACTIVE, EVENT_TAIL } EventState;
typedef enum { EVENT_OK, EVENT_INVALID, EVENT_EXHAUSTED, EVENT_NO_MATCH } EventResult;
typedef enum { EVENT_OPEN, EVENT_RETRIGGER, EVENT_CLOSE, EVENT_EVIDENCE } EventChangeKind;
typedef enum { EVENT_NORMAL, EVENT_GAP, EVENT_LOSS, EVENT_SEQUENCE, EVENT_IDENTITY, EVENT_BAD_INPUT, EVENT_STOP } EventCloseReason;
typedef struct {
 uint64_t id,revision,onset_raw,end_raw,active_raw,trigger_count,triggers[EVENT_TRIGGER_SLOTS];
 unsigned trigger_stored; bool triggers_truncated,closed;
 LfnServiceIdentity identity; double background_power;
 EventCloseReason reason;
 uint64_t spectrum_count,last_spectrum_seq; bool have_spectrum;
 double last_target_power,last_full_power;
 /* Evidence only: no frequency classification threshold has been approved. */
} EventRecord;
typedef struct { EventChangeKind kind; EventRecord event; } EventChange;
typedef struct {
 EventState state; bool exhausted; double background_power; uint64_t background_ticks;
 uint64_t discontinuities,rejected_spectra,expired_pending,pending_overflow; EventRecord current;
} EventSnapshot;
typedef struct {
 EventSnapshot view; double floor_power;
 bool initialized,have_identity,have_delivery,have_energy,have_clock;
 LfnServiceIdentity identity; uint64_t next_delivery,next_seq,next_raw,now,last_end,start_raw;
 uint64_t candidate_raw,tail_raw,next_id; unsigned high_count,low_count;
 struct { uint64_t end,at; double power; } background[EVENT_BACKGROUND_SLOTS];
 unsigned bg_head,bg_count;
 struct { EventRecord record; uint64_t closed_at; } pending[EVENT_PENDING_SLOTS];
 unsigned pending_count;
 EventChange outputs[EVENT_OUTPUT_SLOTS]; unsigned output_head,output_count;
} EventEngine;
/* Caller-owned storage, externally serialized; no DMA ownership/allocation/IO.
 * now_ticks is a nondecreasing nominal 48kHz monotonic model clock across
 * identities, NOT a capture raw counter. Caller converts its monotonic time.
 * floor_power is linear full-scale power; default policy is 0.5e-9.
 * Output/counter exhaustion is sticky and nontransactional: never retry a
 * rejected call. Snapshot retains the last record, including a close that
 * could not enter the output queue. Drain existing changes and record loss
 * before reinit. Pending-evidence overflow instead counts pending_overflow;
 * CLOSE remains available but later evidence cannot revise that record.
 * Snapshots/changes are RAM candidates, never claims of persisted PCM. */
bool event_engine_init(EventEngine *,double floor_power);
EventResult event_on_energy(EventEngine *,const LfnServiceMessage *,uint64_t now_ticks);
EventResult event_on_spectrum(EventEngine *,const LfnSpectrum *,uint64_t now_ticks);
/* Close at the last processed ENERGY end; never invent the unobserved tail.
 * STOP resets threshold/background state but preserves SESSION event IDs. */
EventResult event_engine_finish(EventEngine *,EventCloseReason,uint64_t now_ticks);
bool event_engine_confirmed_end(const EventEngine *,LfnServiceIdentity *,uint64_t *);
/* Storage epoch boundary after all accepted evidence is durable. Pending old
 * captures expire explicitly; new files need not invent old local definitions. */
void event_engine_expire_pending(EventEngine *);
/* Snapshot/copy and take require the same external serialization as inputs. */
bool event_engine_snapshot(const EventEngine *,EventSnapshot *);
bool event_engine_take(EventEngine *,EventChange *);
#endif



