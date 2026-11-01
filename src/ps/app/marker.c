#include "marker.h"
#include <string.h>

static bool stamp_valid(const LfnSampleStamp *s){
  return s&&s->session_id&&s->capture_id&&s->config_id;
}
static bool same_capture(const LfnSampleStamp *a,const LfnSampleStamp *b){
  return a->session_id==b->session_id&&a->capture_id==b->capture_id&&a->config_id==b->config_id;
}
bool lfn_marker_note_valid(const char *note,size_t bytes){
  if(!note||bytes>LFN_MARKER_NOTE_BYTES)return false;
  for(size_t i=0;i<bytes;){
    unsigned char c=(unsigned char)note[i];
    if(c<0x20||c==0x7f)return false;
    if(c<0x80){i++;continue;}
    unsigned need;uint32_t cp,minimum;
    if((c&0xe0)==0xc0){need=1;cp=c&0x1f;minimum=0x80;}
    else if((c&0xf0)==0xe0){need=2;cp=c&0x0f;minimum=0x800;}
    else if((c&0xf8)==0xf0){need=3;cp=c&0x07;minimum=0x10000;}
    else return false;
    if(i+need>=bytes)return false;
    for(unsigned k=1;k<=need;k++){
      unsigned char b=(unsigned char)note[i+k];
      if((b&0xc0)!=0x80)return false;
      cp=(cp<<6)|(uint32_t)(b&0x3f);
    }
    if(cp<minimum||cp>0x10ffff||(cp>=0x80&&cp<=0x9f)||(cp>=0xd800&&cp<=0xdfff))return false;
    i+=need+1;
  }
  return true;
}
void lfn_marker_store_init(LfnMarkerStore *s){
  if(!s)return;
  memset(s,0,sizeof(*s));
  s->next_id=1;
}
LfnMarkerResult lfn_marker_add(LfnMarkerStore *s,const LfnMarkerInput *in){
  if(!s||!in){if(s)s->rejected++;return LFN_MARKER_INVALID;}
  if(s->count>=LFN_MARKER_CAPACITY){s->rejected++;return LFN_MARKER_FULL;}
  if(in->strength<1||in->strength>5||!stamp_valid(&in->start)||
     (in->has_end&&(!stamp_valid(&in->end)||!same_capture(&in->start,&in->end)||
                    in->end.raw_index<in->start.raw_index))||
     !lfn_marker_note_valid(in->note,in->note_bytes)||
     (in->has_client_utc&&!in->sync_map_id)){
    s->rejected++;return LFN_MARKER_INVALID;
  }
  LfnMarker *m=&s->items[s->count];
  memset(m,0,sizeof(*m));
  m->marker_id=s->next_id;
  m->start=in->start;m->end=in->end;m->has_end=in->has_end;
  m->strength=in->strength;
  m->submit_monotonic_ns=in->submit_monotonic_ns;
  m->client_utc_ns=in->has_client_utc?in->client_utc_ns:0;
  m->has_client_utc=in->has_client_utc;
  m->sync_map_id=in->has_client_utc?in->sync_map_id:0;
  m->uncertainty_ns=in->has_client_utc?in->uncertainty_ns:0;
  if(in->note_bytes)memcpy(m->note,in->note,in->note_bytes);
  s->count++;s->next_id++;
  return LFN_MARKER_ADDED;
}
const LfnMarker *lfn_marker_find(const LfnMarkerStore *s,uint64_t marker_id){
  if(!s||!marker_id)return 0;
  for(unsigned i=0;i<s->count;i++)if(s->items[i].marker_id==marker_id)return &s->items[i];
  return 0;
}
