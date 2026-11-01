#ifndef LFN_EVENT_FEED_H
#define LFN_EVENT_FEED_H
#include "event_engine.h"
#include <stdatomic.h>

enum { EVENT_FEED_SLOTS=128 };
typedef struct {uint64_t event_id;LfnServiceIdentity identity;uint64_t end_raw;} EventFeedProgress;
typedef struct EventFeed {
 /* Capture task owns the engine and all non-atomic fields except the inbox
  * consumer cursor. It never calls storage or waits for the storage task. */
 EventEngine engine;uint64_t session,next_delivery;
 bool have_spectrum;uint64_t spectrum_session,spectrum_capture,spectrum_seq;uint32_t spectrum_config;
 EventChange inbox[EVENT_FEED_SLOTS];atomic_uint written,read;
 atomic_int error;atomic_bool closed;
 atomic_flag progress_lock;EventFeedProgress progress;bool have_progress;
} EventFeed;
/* Initialize only with both owners stopped. Storage consumes before reuse;
 * queued changes and SESSION event IDs must not be reset during a capture. */
bool event_feed_init(EventFeed *,uint64_t session);
/* Capture-only inputs; times are nondecreasing microseconds, not raw samples.
 * Energy has its own delivery ordinals, independent of SD admission/loss.
 * A full output inbox latches EXHAUSTED; already published changes survive.
 * finish is a producer barrier: subsequent spectra are ignored until ENERGY
 * begins a new capture, so closed plus empty is a valid consumer drain. */
EventResult event_feed_energy(EventFeed *,const LfnServiceIdentity *,const CaptureEnergyMessage *,uint64_t now_us);
EventResult event_feed_spectrum(EventFeed *,const LfnSpectrum *,uint64_t now_us);
EventResult event_feed_finish(EventFeed *,EventCloseReason,uint64_t now_us);
/* Storage-only consumer. Peek does not release a slot; leave a new OPEN
 * queued until the preceding event is durably closed. */
bool event_feed_peek(EventFeed *,EventChange *);
bool event_feed_take(EventFeed *,EventChange *);
/* Short try-lock copy; busy leaves out untouched. Last published progress may
 * lag a skipped producer update. Progress is for exactly
 * event_id/identity, never authorization to extend a different queued event. */
bool event_feed_progress(EventFeed *,EventFeedProgress *);
unsigned event_feed_pending(const EventFeed *);
EventResult event_feed_error(const EventFeed *);
bool event_feed_closed(const EventFeed *);
#endif
