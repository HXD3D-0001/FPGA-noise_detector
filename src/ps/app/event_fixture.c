#include "event_fixture.h"
#include <string.h>

int32_t event_fixture_sample(uint64_t sample){
 int32_t amplitude=sample>=EVENT_FIXTURE_BACKGROUND&&sample<EVENT_FIXTURE_BACKGROUND+EVENT_FIXTURE_HIGH?
  EVENT_FIXTURE_HIGH_AMPLITUDE:EVENT_FIXTURE_LOW_AMPLITUDE;
 return sample&1?-amplitude:amplitude;
}
uint64_t event_fixture_energy(uint64_t first){
 int64_t sum=0;uint64_t squares=0;
 for(unsigned k=0;k<256;k++){
  int64_t value=event_fixture_sample(first+k);sum+=value;squares+=(uint64_t)(value*value);
 }
 return 256*squares-(uint64_t)(sum*sum);
}
bool event_fixture_init(EventFixture *f,const LfnServiceIdentity *identity,uint64_t start,
 uint64_t origin,const EventFixtureHooks *hooks){
 const uint64_t total=(EVENT_FIXTURE_BACKGROUND+EVENT_FIXTURE_HIGH+EVENT_FIXTURE_TAIL)/256*256;
 if(!f||!identity||!hooks||!hooks->pcm||!hooks->energy||!identity->session_id||!identity->capture_id||
    !identity->config.config_id||identity->config.mode||identity->config.source!=1||
    origin>UINT64_MAX-total*48||start>UINT64_MAX-total*1000)return false;
 memset(f,0,sizeof(*f));f->identity=*identity;f->hooks=*hooks;f->start_us=start;
 f->origin_raw=origin;f->now_us=start;f->total_samples=total;return true;
}
EventFixtureResult event_fixture_step(EventFixture *f,uint64_t now){
 if(!f||f->failed)return FIXTURE_ERROR;
 if(now<f->now_us){f->failed=true;return FIXTURE_ERROR;}f->now_us=now;
 if(f->stage==0){
  if(f->next_sample==f->total_samples)return FIXTURE_DONE;
  if(now-f->start_us<(f->next_sample+256)*1000)return FIXTURE_WAIT;
  for(unsigned k=0;k<256;k++)f->samples[k]=event_fixture_sample(f->next_sample+k);
  PacketHeader header={.type=LFN_PCM,.flags=LFN_VALID,.capture_id=f->identity.capture_id,
   .stream_seq=f->next_sample/256,.first_raw_sample_index=f->origin_raw+f->next_sample*48,
   .config_id=f->identity.config.config_id};
  if(!packet_encode(&header,f->samples,256,f->packet,sizeof(f->packet),&f->packet_bytes)){
   f->failed=true;return FIXTURE_ERROR;
  }
  f->stage=1;return FIXTURE_PROGRESS;
 }
 if(f->stage==1){
  PacketView packet;
  if(!packet_decode(f->packet,f->packet_bytes,&packet)){f->failed=true;return FIXTURE_ERROR;}
  if(!f->hooks.pcm(f->hooks.context,&f->identity,&packet))return FIXTURE_BUSY;
  f->pcm_count++;f->energy_outputs=f->next_sample?2:1;f->energy_index=0;f->stage=2;
  return FIXTURE_PROGRESS;
 }
 uint64_t first=f->next_sample-(f->next_sample?128:0)+f->energy_index*128;
 memset(&f->energy,0,sizeof(f->energy));
 f->energy.record=(PlEnergy){.seq=f->energy_seq,.capture_id=f->identity.capture_id,
  .first_raw=f->origin_raw+first*48,.energy=event_fixture_energy(first),
  .config_id=f->identity.config.config_id,.flags=LFN_VALID};
 if(!f->hooks.energy(f->hooks.context,&f->identity,&f->energy))return FIXTURE_BUSY;
 f->energy_seq++;f->energy_count++;
 if(++f->energy_index==f->energy_outputs){f->next_sample+=256;f->stage=0;}
 return FIXTURE_PROGRESS;
}
