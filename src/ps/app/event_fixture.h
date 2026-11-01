#ifndef LFN_EVENT_FIXTURE_H
#define LFN_EVENT_FIXTURE_H
#include "service_bridge.h"

enum { EVENT_FIXTURE_BACKGROUND=64000,EVENT_FIXTURE_HIGH=901000,
 EVENT_FIXTURE_TAIL=25000,EVENT_FIXTURE_LOW_AMPLITUDE=262144,
 EVENT_FIXTURE_HIGH_AMPLITUDE=1048576 };
typedef enum { FIXTURE_WAIT,FIXTURE_PROGRESS,FIXTURE_BUSY,FIXTURE_DONE,FIXTURE_ERROR } EventFixtureResult;
typedef struct {
 void *context;
 /* Synchronous copy only. false retries the same output and therefore requires
  * no prior side effect. An offer that already records dropped/lost input is
  * consumed: return true, preserve that sink's explicit loss accounting. */
 bool (*pcm)(void *,const LfnServiceIdentity *,const PacketView *);
 bool (*energy)(void *,const LfnServiceIdentity *,const CaptureEnergyMessage *);
} EventFixtureHooks;
typedef struct {
 LfnServiceIdentity identity;EventFixtureHooks hooks;
 uint64_t start_us,origin_raw,next_sample,energy_seq,now_us,total_samples;
 uint64_t pcm_count,energy_count;unsigned stage,energy_index,energy_outputs;
 bool failed;size_t packet_bytes;
 uint8_t packet[LFN_HEADER_BYTES+LFN_PCM_COUNT*4];
 int32_t samples[LFN_PCM_COUNT];CaptureEnergyMessage energy;
} EventFixture;
/* Explicit PS_SYNTHETIC_FIXTURE only. Never attach this to an ADC/PL capture
 * or claim CAP1 time-pair provenance. Use legacy UNCONFIRMED fixture CAPTURE.
 * Full 256-sample PCM packets only; final <256 samples are not generated.
 * clock is PS monotonic time; now_us must be real time for a board claim. */
bool event_fixture_init(EventFixture *,const LfnServiceIdentity *,uint64_t start_us,
 uint64_t origin_raw,const EventFixtureHooks *);
/* At most one output callback and one <=256-sample generation per call. No I/O, wait,
 * allocation or synthetic advancement of the supplied monotonic clock. */
EventFixtureResult event_fixture_step(EventFixture *,uint64_t now_us);
/* Shared exact waveform and de-meaned 256-point integer energy oracle. */
int32_t event_fixture_sample(uint64_t sample);
uint64_t event_fixture_energy(uint64_t first_sample);
#endif
