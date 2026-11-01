#ifndef LFN_EVENT_FIXTURE_IMAGE_H
#define LFN_EVENT_FIXTURE_IMAGE_H
#include <stdbool.h>
#include <stdint.h>
typedef struct {
 uint64_t session,elapsed_us,samples,pcm_packets,energy_windows,events,segments;
 uint64_t event_file_id,mon_synced,event_synced,max_source_lag_us;
 uint32_t stage,error,roll_state,roll_error,factory_state,engine_state,stack_free_words;
 bool started,running,closed,failed;char directory[16];
} EventFixtureImageHealth;
/* Independent, one-shot image. Call FXBEGIN with a fresh host nonce after the
 * scheduler starts. No PL/DMA start, boot writes, overwrite or automatic retry. */
bool lfn_event_fixture_image_start(uint64_t session);
bool lfn_event_fixture_image_health(EventFixtureImageHealth *);
extern EventFixtureImageHealth lfn_event_fixture_health;
/* [workspace bytes, history bytes, runtime bytes, source bytes, task stack bytes]. */
extern const uint32_t lfn_event_fixture_sizes[5];
#endif
