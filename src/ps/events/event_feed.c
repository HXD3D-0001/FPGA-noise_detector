#include "event_feed.h"
#include <string.h>

static uint64_t ticks(uint64_t us){return us/125*6+(us%125)*6/125;}
static EventResult fail(EventFeed*f,EventResult result){
 if(result!=EVENT_OK&&result!=EVENT_NO_MATCH)atomic_store_explicit(&f->error,result,memory_order_release);
 return result;
}
static void progress_publish(EventFeed*f){
 if(atomic_flag_test_and_set_explicit(&f->progress_lock,memory_order_acquire))return;
 EventEngine*e=&f->engine;
 f->have_progress=(e->view.state==EVENT_ACTIVE||e->view.state==EVENT_TAIL)&&e->have_energy;
 if(f->have_progress)f->progress=(EventFeedProgress){e->view.current.id,e->identity,e->last_end};
 atomic_flag_clear_explicit(&f->progress_lock,memory_order_release);
}
static EventResult publish(EventFeed*f,EventResult result){
 EventChange change;
 while(event_engine_take(&f->engine,&change)){
  unsigned written=atomic_load_explicit(&f->written,memory_order_relaxed);
  unsigned read=atomic_load_explicit(&f->read,memory_order_acquire);
  if(written-read==EVENT_FEED_SLOTS)return fail(f,EVENT_EXHAUSTED);
  f->inbox[written%EVENT_FEED_SLOTS]=change;
  atomic_store_explicit(&f->written,written+1,memory_order_release);
 }
 progress_publish(f);return fail(f,result);
}
bool event_feed_init(EventFeed*f,uint64_t session){
 if(!f||!session)return false;
 memset(f,0,sizeof(*f));f->session=session;f->next_delivery=1;
 atomic_init(&f->written,0);atomic_init(&f->read,0);atomic_init(&f->error,EVENT_OK);atomic_init(&f->closed,true);
 atomic_flag_clear(&f->progress_lock);return event_engine_init(&f->engine,5e-10);
}
EventResult event_feed_error(const EventFeed*f){return f?(EventResult)atomic_load_explicit(&f->error,memory_order_acquire):EVENT_INVALID;}
bool event_feed_closed(const EventFeed*f){return f&&atomic_load_explicit(&f->closed,memory_order_acquire);}
unsigned event_feed_pending(const EventFeed*f){
 if(!f)return 0;
 unsigned read=atomic_load_explicit(&f->read,memory_order_acquire);
 return atomic_load_explicit(&f->written,memory_order_acquire)-read;
}
EventResult event_feed_energy(EventFeed*f,const LfnServiceIdentity*identity,const CaptureEnergyMessage*energy,uint64_t now){
 if(!f)return EVENT_INVALID;
 EventResult error=event_feed_error(f);if(error!=EVENT_OK)return error;
 if(!identity||!energy||identity->session_id!=f->session)return fail(f,EVENT_INVALID);
 if(f->next_delivery==UINT64_MAX)return fail(f,EVENT_EXHAUSTED);
 if(f->engine.have_identity&&(identity->capture_id!=f->engine.identity.capture_id||identity->config.config_id!=f->engine.identity.config.config_id)){
  event_engine_expire_pending(&f->engine);f->have_spectrum=false;
 }
 atomic_store_explicit(&f->closed,false,memory_order_release);
 LfnServiceMessage message={0};message.kind=SERVICE_ENERGY;
 message.first_delivery=f->next_delivery++;message.end_delivery=f->next_delivery;
 message.identity=*identity;message.energy=*energy;
 return publish(f,event_on_energy(&f->engine,&message,ticks(now)));
}
EventResult event_feed_spectrum(EventFeed*f,const LfnSpectrum*spectrum,uint64_t now){
 if(!f)return EVENT_INVALID;
 EventResult error=event_feed_error(f);if(error!=EVENT_OK)return error;
 if(!spectrum||spectrum->session_id!=f->session)return fail(f,EVENT_INVALID);
 /* closed is a producer barrier: once STOP is published, no later snapshot
  * may append evidence behind the consumer's closed-and-empty observation. */
 if(event_feed_closed(f))return EVENT_NO_MATCH;
 if(f->have_spectrum&&f->spectrum_session==spectrum->session_id&&f->spectrum_capture==spectrum->capture_id&&
    f->spectrum_config==spectrum->config_id&&spectrum->stream_seq<=f->spectrum_seq)return EVENT_NO_MATCH;
 EventResult result=event_on_spectrum(&f->engine,spectrum,ticks(now));
 if(result==EVENT_OK||result==EVENT_NO_MATCH){
  f->have_spectrum=true;f->spectrum_session=spectrum->session_id;f->spectrum_capture=spectrum->capture_id;
  f->spectrum_config=spectrum->config_id;f->spectrum_seq=spectrum->stream_seq;
 }
 return publish(f,result);
}
EventResult event_feed_finish(EventFeed*f,EventCloseReason reason,uint64_t now){
 if(!f)return EVENT_INVALID;
 EventResult error=event_feed_error(f);if(error!=EVENT_OK)return error;
 if(event_feed_closed(f))return EVENT_OK;
 EventResult result=publish(f,event_engine_finish(&f->engine,reason,ticks(now)));
 if(result==EVENT_OK)atomic_store_explicit(&f->closed,true,memory_order_release);
 return result;
}
bool event_feed_peek(EventFeed*f,EventChange*out){
 if(!f||!out)return false;
 unsigned read=atomic_load_explicit(&f->read,memory_order_relaxed);
 if(read==atomic_load_explicit(&f->written,memory_order_acquire))return false;
 *out=f->inbox[read%EVENT_FEED_SLOTS];return true;
}
bool event_feed_take(EventFeed*f,EventChange*out){
 if(!event_feed_peek(f,out))return false;
 unsigned read=atomic_load_explicit(&f->read,memory_order_relaxed);
 atomic_store_explicit(&f->read,read+1,memory_order_release);return true;
}
bool event_feed_progress(EventFeed*f,EventFeedProgress*out){
 if(!f||!out||atomic_flag_test_and_set_explicit(&f->progress_lock,memory_order_acquire))return false;
 bool have=f->have_progress;if(have)*out=f->progress;
 atomic_flag_clear_explicit(&f->progress_lock,memory_order_release);return have;
}
