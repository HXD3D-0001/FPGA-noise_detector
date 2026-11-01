#include "storage_records.h"
#include <float.h>
#include <string.h>
_Static_assert(sizeof(float)==4&&FLT_RADIX==2&&FLT_MANT_DIG==24&&FLT_MAX_EXP==128,"Require IEEE754 float32 PSD");
static bool device(const char*s){
 if(!s||!*s)return false;
 for(unsigned i=0;i<64;i++){unsigned char c=(unsigned char)s[i];if(!c)return true;
  if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.'))return false;
 }return false;
}
static bool policy(const StorageRecordPolicy*p){return p&&device(p->device_id)&&p->session_id&&p->capture_id&&p->config.config_id&&p->config.mode<3&&p->config.source<2&&p->config.channel<2&&p->config.pattern<4;}
static bool identity(const StorageRecordPolicy*p,const RecordIdentity*i){
 return policy(p)&&i&&i->session_id==p->session_id&&i->capture_id==p->capture_id&&i->config_id==p->config.config_id&&
 (i->flags&LFN_VALID)&&!(i->flags&~(uint16_t)(LFN_VALID|LFN_SATURATION))&&i->first_raw<i->end_raw;
}
static bool grid(const StorageRecordPolicy*p,unsigned k,uint64_t first,uint64_t hop,uint64_t*seq){
 if(!(p->bound_mask&(1u<<k))||first<p->first_raw[k])return false;
 uint64_t delta=first-p->first_raw[k];if(delta%hop||delta/hop>UINT64_MAX-p->first_seq[k])return false;
 *seq=p->first_seq[k]+delta/hop;return *seq!=UINT64_MAX;
}
static uint32_t le32(const uint8_t*b){return (uint32_t)b[0]|((uint32_t)b[1]<<8)|((uint32_t)b[2]<<16)|((uint32_t)b[3]<<24);}
static void put32(uint8_t*b,uint32_t v){for(unsigned i=0;i<4;i++)b[i]=(uint8_t)(v>>(8*i));}
typedef struct {char text[512];size_t n;bool ok;} Json;
static void add(Json*j,const char*s){size_t n=strlen(s);if(n>=sizeof(j->text)-j->n){j->ok=false;return;}memcpy(j->text+j->n,s,n);j->n+=n;}
static void number(Json*j,uint64_t n,bool quoted){char b[21];unsigned used=0;if(quoted)add(j,"\"");do{b[used++]=(char)('0'+n%10);n/=10;}while(n);while(used){char c[2]={b[--used],0};add(j,c);}if(quoted)add(j,"\"");}
static bool energy_json(const StorageRecordPolicy*p,uint64_t seq,uint64_t energy,uint16_t flags,Json*j){
 memset(j,0,sizeof(*j));j->ok=true;add(j,"{\"schema_version\":1,\"device_id\":\"");add(j,p->device_id);add(j,"\",\"session_id\":");number(j,p->session_id,true);
 add(j,",\"capture_id\":");number(j,p->capture_id,true);add(j,",\"config_id\":");number(j,p->config.config_id,false);
 add(j,",\"energy_seq\":");number(j,seq,true);add(j,",\"E_u64\":");number(j,energy,true);
 add(j,",\"window_count\":256,\"hop_count\":128,\"raw_stride\":48,\"flags\":");number(j,flags,false);add(j,"}");return j->ok;
}
typedef struct {const uint8_t*p,*end;} Cursor;
static bool take(Cursor*c,const char*s){size_t n=strlen(s);if(n>(size_t)(c->end-c->p)||memcmp(c->p,s,n))return false;c->p+=n;return true;}
static bool integer(Cursor*c,bool quoted,uint64_t*out){
 if(quoted&&!take(c,"\""))return false;
 const uint8_t*start=c->p;uint64_t v=0;
 while(c->p<c->end&&*c->p>='0'&&*c->p<='9'){unsigned d=*c->p-'0';if(v>(UINT64_MAX-d)/10)return false;v=v*10+d;c->p++;}
 if(c->p==start||(c->p-start>1&&*start=='0')||(quoted&&!take(c,"\"")))return false;
 *out=v;return true;
}
bool storage_data_validate(void*context,const RecordIdentity*i,const uint8_t*b,size_t n){
 const StorageRecordPolicy*p=context;if(!identity(p,i)||!b)return false;
 if(i->type==RECORD_ENERGY){
  uint64_t expected=0,session=0,capture=0,config=0,seq=0,energy=0,flags=0;
  if(p->config.mode||i->end_raw-i->first_raw!=12288||!grid(p,3,i->first_raw,6144,&expected)||n>511)return false;
  Cursor c={b,b+n};if(!take(&c,"{\"schema_version\":1,\"device_id\":\"")||!take(&c,p->device_id)||!take(&c,"\",\"session_id\":")||!integer(&c,true,&session)||
   !take(&c,",\"capture_id\":")||!integer(&c,true,&capture)||!take(&c,",\"config_id\":")||!integer(&c,false,&config)||
   !take(&c,",\"energy_seq\":")||!integer(&c,true,&seq)||!take(&c,",\"E_u64\":")||!integer(&c,true,&energy)||
   !take(&c,",\"window_count\":256,\"hop_count\":128,\"raw_stride\":48,\"flags\":")||!integer(&c,false,&flags)||!take(&c,"}")||c.p!=c.end)return false;
  return session==p->session_id&&capture==p->capture_id&&config==p->config.config_id&&seq==expected&&energy<=(UINT64_C(1)<<62)&&flags==i->flags;
 }
 if(i->type==RECORD_PSD){
  uint64_t seq=0;if(p->config.mode||n!=LFN_SPECTRUM_BINS*4||i->end_raw-i->first_raw!=196608||!grid(p,0,i->first_raw,98304,&seq))return false;
  for(size_t k=0;k<n;k+=4){uint32_t v=le32(b+k);if((v&0x7f800000)==0x7f800000||((v&0x80000000)&&(v&0x7fffffff)))return false;}return true;
 }
 if(i->type==RECORD_PCM){
  unsigned k=p->config.mode?2:1;uint64_t stride=p->config.mode?1:48;
  if(!(p->bound_mask&(1u<<k))||i->first_raw<p->first_raw[k]||(i->first_raw-p->first_raw[k])%stride||!n||n%4||n>4096*4||i->end_raw-i->first_raw!=(n/4)*stride)return false;
  for(size_t a=0;a<n;a+=4){uint32_t v=le32(b+a);if(v>0x007fffff&&v<0xff800000)return false;}return true;
 }
 return false;
}
static void result(StorageRequest*out,const RecordIdentity*i,const uint8_t*b,size_t n){memset(out,0,sizeof(*out));out->identity=*i;out->payload=b;out->payload_bytes=n;}
bool storage_energy_record(const StorageRecordPolicy*p,const LfnServiceMessage*m,StorageRequest*out,uint8_t*b,size_t cap){
 if(!policy(p)||!m||!out||!b||m->kind!=SERVICE_ENERGY||m->energy.gap||!m->first_delivery||m->first_delivery==UINT64_MAX||m->end_delivery!=m->first_delivery+1||
 m->identity.session_id!=p->session_id||m->identity.capture_id!=p->capture_id||m->identity.config.config_id!=p->config.config_id||
 m->identity.config.mode!=p->config.mode||m->identity.config.source!=p->config.source||m->identity.config.channel!=p->config.channel||m->identity.config.pattern!=p->config.pattern)return false;
 const PlEnergy*e=&m->energy.record;if(e->capture_id!=p->capture_id||e->config_id!=p->config.config_id||e->first_raw>UINT64_MAX-12288)return false;
 RecordIdentity id={RECORD_ENERGY,(uint16_t)e->flags,e->config_id,p->session_id,e->capture_id,e->first_raw,e->first_raw+12288,0};
 if(e->flags>UINT16_MAX)return false;
 Json j;if(!energy_json(p,e->seq,e->energy,id.flags,&j)||j.n>cap||!storage_data_validate((void*)p,&id,(const uint8_t*)j.text,j.n))return false;
 memcpy(b,j.text,j.n);result(out,&id,b,j.n);return true;
}
bool storage_psd_record(const StorageRecordPolicy*p,const LfnSpectrum*s,StorageRequest*out,uint8_t*b,size_t cap){
 if(!policy(p)||!s||!s->valid||!out||!b||cap<LFN_SPECTRUM_BINS*4||s->test_source!=(p->config.source==PL_CAPTURE_TEST))return false;
 RecordIdentity id={RECORD_PSD,s->flags,s->config_id,s->session_id,s->capture_id,s->first_raw,s->end_raw,0};uint64_t seq=0;
 if(!identity(p,&id)||p->config.mode||id.end_raw-id.first_raw!=196608||!grid(p,0,id.first_raw,98304,&seq)||seq!=s->stream_seq)return false;
 for(unsigned i=0;i<LFN_SPECTRUM_BINS;i++){uint32_t v;memcpy(&v,&s->psd[i],4);if((v&0x7f800000)==0x7f800000||((v&0x80000000)&&(v&0x7fffffff)))return false;}
 for(unsigned i=0;i<LFN_SPECTRUM_BINS;i++){uint32_t v;memcpy(&v,&s->psd[i],4);put32(b+i*4,v);}result(out,&id,b,LFN_SPECTRUM_BINS*4);return true;
}
bool storage_pcm_record(const StorageRecordPolicy*p,const PacketView*v,size_t offset,size_t count,StorageRequest*out,uint8_t*b,size_t cap){
 if(!policy(p)||!v||!out||!b||!count||count>4096||cap<count*4||offset>v->value_count||count>v->value_count-offset||v->header.type!=(p->config.mode?LFN_RAW:LFN_PCM))return false;
 uint64_t stride=p->config.mode?1:48;if(offset>(UINT64_MAX-v->header.first_raw_sample_index)/stride)return false;
 uint64_t first=v->header.first_raw_sample_index+offset*stride;if(count>(UINT64_MAX-first)/stride)return false;
 RecordIdentity id={RECORD_PCM,v->header.flags,v->header.config_id,p->session_id,v->header.capture_id,first,first+count*stride,0};
 unsigned k=p->config.mode?2:1;uint64_t seq=0,hop=p->config.mode?8192:12288;
 if(!identity(p,&id)||!grid(p,k,v->header.first_raw_sample_index,hop,&seq)||seq!=v->header.stream_seq)return false;
 for(size_t i=0;i<count;i++){int32_t value;if(!packet_value(v,offset+i,&value)||value<(-8388607-1)||value>8388607)return false;}
 for(size_t i=0;i<count;i++){int32_t value;(void)packet_value(v,offset+i,&value);put32(b+i*4,(uint32_t)value);}result(out,&id,b,count*4);return true;
}

bool storage_pcm_samples_record(const StorageRecordPolicy*p,const StoragePcmSamples*s,StorageRequest*out,uint8_t*b,size_t cap){
 if(!policy(p)||p->config.mode||!s||!out||!b||!s->samples||!s->count||s->count>4096||cap<s->count*4||s->first_raw>UINT64_MAX-s->count*48)return false;
 RecordIdentity id={RECORD_PCM,s->flags,s->config_id,s->session_id,s->capture_id,s->first_raw,s->first_raw+s->count*48,0};
 if(!identity(p,&id)||!(p->bound_mask&2)||id.first_raw<p->first_raw[1]||(id.first_raw-p->first_raw[1])%48)return false;
 for(size_t k=0;k<s->count;k++)if(s->samples[k]<-8388608||s->samples[k]>8388607)return false;
 for(size_t k=0;k<s->count;k++)put32(b+4*k,(uint32_t)s->samples[k]);
 result(out,&id,b,s->count*4);return true;
}
