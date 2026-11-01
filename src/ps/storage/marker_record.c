#include "marker_record.h"
#include <stdio.h>
#include <string.h>
static bool device_valid(const char*s){
 if(!s||!*s)return false;
 for(unsigned k=0;k<64;k++){unsigned char c=(unsigned char)s[k];if(!c)return true;
  if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.'))return false;
 }return false;
}
static bool valid(const LfnMarker*m,size_t *bytes){
 if(!m||!m->marker_id||!m->start.session_id||!m->start.capture_id||!m->start.config_id||m->strength<1||m->strength>5)return false;
 if(m->has_end&&(m->end.session_id!=m->start.session_id||m->end.capture_id!=m->start.capture_id||m->end.config_id!=m->start.config_id||m->end.raw_index<m->start.raw_index||m->end.group_delay_compensated!=m->start.group_delay_compensated))return false;
 if((m->has_client_utc&&!m->sync_map_id)||(!m->has_client_utc&&(m->client_utc_ns||m->sync_map_id||m->uncertainty_ns)))return false;
 size_t n=0;while(n<sizeof(m->note)&&m->note[n])n++;
 if(!lfn_marker_note_valid(m->note,n))return false;
 if(bytes)*bytes=n;
 return true;
}
bool storage_marker_record(const char*device,const LfnMarker*m,StorageRequest*out,uint8_t*b,size_t capacity){
 size_t note_bytes;if(!device_valid(device)||!out||!b||!valid(m,&note_bytes))return false;
 char utc[24];if(m->has_client_utc)(void)snprintf(utc,sizeof(utc),"\"%lld\"",(long long)m->client_utc_ns);else strcpy(utc,"null");
 uint64_t end=m->has_end?m->end.raw_index:m->start.raw_index;
 int n=snprintf((char*)b,capacity,"{\"schema_version\":1,\"device_id\":\"%s\",\"session_id\":\"%llu\",\"capture_id\":\"%llu\",\"config_id\":%u,\"marker_id\":\"%llu\",\"start_raw\":\"%llu\",\"end_raw\":\"%llu\",\"has_end\":%s,\"strength\":%u,\"submit_monotonic_ns\":\"%llu\",\"has_client_utc\":%s,\"client_utc_ns\":%s,\"sync_map_id\":\"%llu\",\"uncertainty_ns\":\"%llu\",\"start_group_delay_compensated\":%s,\"end_group_delay_compensated\":%s,\"note\":\"",
 device,(unsigned long long)m->start.session_id,(unsigned long long)m->start.capture_id,(unsigned)m->start.config_id,(unsigned long long)m->marker_id,(unsigned long long)m->start.raw_index,(unsigned long long)end,m->has_end?"true":"false",(unsigned)m->strength,(unsigned long long)m->submit_monotonic_ns,m->has_client_utc?"true":"false",utc,(unsigned long long)m->sync_map_id,(unsigned long long)m->uncertainty_ns,m->start.group_delay_compensated?"true":"false",m->start.group_delay_compensated?"true":"false");
 if(n<0||(size_t)n>=capacity)return false;
 size_t used=(size_t)n;
 for(size_t k=0;k<note_bytes;k++){unsigned char c=(unsigned char)m->note[k];size_t need=c=='"'||c=='\\'?2:1;if(need>=capacity-used)return false;if(need==2)b[used++]='\\';b[used++]=c;}
 if(capacity-used<3)return false;
 b[used++]='"';b[used++]='}';b[used]=0;
 memset(out,0,sizeof(*out));out->identity=(RecordIdentity){RECORD_MARKER,0,m->start.config_id,m->start.session_id,m->start.capture_id,m->start.raw_index,end,0};out->payload=b;out->payload_bytes=used;return true;
}
static unsigned hex(unsigned char c){return c<='9'?c-'0':c<='F'?c-'A'+10:c-'a'+10;}
static uint32_t code(const uint8_t*p){return (hex(p[0])<<12)|(hex(p[1])<<8)|(hex(p[2])<<4)|hex(p[3]);}
/* The shared parser has already checked escape syntax and surrogate pairs. */
bool storage_marker_note_decode(const MetadataJson*j,int t,char*out){
 if(t<0||j->token[t].type!=MJ_STRING)return false;
 size_t at=j->token[t].start,end=j->token[t].end,n=0;
 while(at<end){uint32_t c=j->bytes[at++];
  if(c=='\\'){
   c=j->bytes[at++];
   if(c=='u'){c=code(j->bytes+at);at+=4;if(c>=0xd800&&c<=0xdbff){uint32_t low=code(j->bytes+at+2);at+=6;c=0x10000+((c-0xd800)<<10)+(low-0xdc00);}
    unsigned len=c<0x80?1:c<0x800?2:c<0x10000?3:4;if(n+len>LFN_MARKER_NOTE_BYTES)return false;
    if(len==1)out[n++]=(char)c;else{out[n++]=(char)((len==2?0xc0:len==3?0xe0:0xf0)|(c>>(6*(len-1))));for(unsigned k=len-1;k>0;k--)out[n++]=(char)(0x80|((c>>(6*(k-1)))&63));}continue;
   }
   if(c!='"'&&c!='\\'&&c!='/')return false;
  }
  if(n==LFN_MARKER_NOTE_BYTES)return false;
  out[n++]=(char)c;
 }
 out[n]=0;return lfn_marker_note_valid(out,n);
}
#define F(name) metadata_json_field(j,0,name)
bool storage_marker_decode(const RecordIdentity*i,const uint8_t*b,size_t n,MetadataJson*j,LfnMarker*out){
 if(!i||i->type!=RECORD_MARKER||i->flags||!j||!out||!metadata_json_parse(j,b,n))return false;
 LfnMarker m={0};uint32_t version;char device[64];
 if(!metadata_json_u32(j,F("schema_version"),&version)||version!=1||!metadata_json_string(j,F("device_id"),device,sizeof(device))||!device_valid(device)||
 !metadata_json_u64(j,F("session_id"),&m.start.session_id)||!metadata_json_u64(j,F("capture_id"),&m.start.capture_id)||!metadata_json_u32(j,F("config_id"),&m.start.config_id)||
 !metadata_json_u64(j,F("marker_id"),&m.marker_id)||!metadata_json_u64(j,F("start_raw"),&m.start.raw_index)||!metadata_json_bool(j,F("has_end"),&m.has_end)||
 !metadata_json_bool(j,F("start_group_delay_compensated"),&m.start.group_delay_compensated)||!metadata_json_u32(j,F("strength"),&m.strength)||
 !metadata_json_u64(j,F("submit_monotonic_ns"),&m.submit_monotonic_ns)||!metadata_json_bool(j,F("has_client_utc"),&m.has_client_utc)||
 !metadata_json_u64(j,F("sync_map_id"),&m.sync_map_id)||!metadata_json_u64(j,F("uncertainty_ns"),&m.uncertainty_ns)||!storage_marker_note_decode(j,F("note"),m.note))return false;
 m.end=m.start;
 if(!metadata_json_u64(j,F("end_raw"),&m.end.raw_index)||!metadata_json_bool(j,F("end_group_delay_compensated"),&m.end.group_delay_compensated)||
 (m.has_client_utc?!metadata_json_i64(j,F("client_utc_ns"),&m.client_utc_ns):!metadata_json_is(j,F("client_utc_ns"),"null"))||
 (!m.has_end&&(m.end.raw_index!=m.start.raw_index||m.end.group_delay_compensated!=m.start.group_delay_compensated))||!valid(&m,NULL)||
 i->session_id!=m.start.session_id||i->capture_id!=m.start.capture_id||i->config_id!=m.start.config_id||i->first_raw!=m.start.raw_index||i->end_raw!=m.end.raw_index)return false;
 *out=m;return true;
}
