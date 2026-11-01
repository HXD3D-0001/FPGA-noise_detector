#include "network_extra.h"
#include "../storage/metadata_json.h"
#include "../storage/marker_record.h"
#include <stdio.h>
#include <string.h>
static MetadataJson json; /* HTTP task only; keep parser scratch off its stack. */
static bool take(NetworkExtra*n){return !atomic_flag_test_and_set_explicit(&n->lock,memory_order_acquire);}
static void give(NetworkExtra*n){atomic_flag_clear_explicit(&n->lock,memory_order_release);}
void network_extra_init(NetworkExtra*n,const NetworkExtraHooks*h){memset(n,0,sizeof(*n));atomic_flag_clear(&n->lock);atomic_init(&n->marker.state,0);atomic_init(&n->report.state,0);n->hooks=*h;}
static bool token(const char*p,uint64_t*value){uint64_t n=0;if(!*p)return false;for(;*p;p++){if(*p<'0'||*p>'9'||n>(UINT64_MAX-(unsigned)(*p-'0'))/10)return false;n=n*10+(unsigned)(*p-'0');}*value=n;return n!=0;}
static bool hex6(const char*p){for(unsigned k=0;k<6;k++){char c=p[k];if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F')))return false;}return true;}
static bool report_path(const char*p){size_t n=strlen(p);if(!n)return true;if(n!=17&&n!=25)return false;if(strncmp(p,"SR",2)||!hex6(p+2)||p[8]!='/')return false;p+=9;if(n==25){if(p[0]!='E'||!hex6(p+1)||p[7]!='/')return false;p+=8;}return !strcmp(p,"ASUM.LFR")||!strcmp(p,"BSUM.LFR");}
static bool text_field(const char*key,char*out,size_t cap){int t=metadata_json_field(&json,0,key);if(t<0){out[0]=0;return true;}return metadata_json_string(&json,t,out,cap);}
static int status(NetworkExtra*n,bool marker,char*out,size_t cap){
 int st=atomic_load_explicit(marker?&n->marker.state:&n->report.state,memory_order_acquire);
 uint64_t request=marker?n->marker.request:n->report.request;int written;
 if(st==3&&!marker){size_t len=strlen(n->report.data);if(len>=cap)return 503;memcpy(out,n->report.data,len+1);return 200;}
 if(st==4)written=snprintf(out,cap,"{\"state\":\"ERROR\",\"request_id\":\"%llu\",\"error\":\"%s\",\"code\":%d}",(unsigned long long)request,marker?"MARKER_REJECTED":"REPORT_NOT_AVAILABLE",marker?n->marker.error:n->report.error);
 else if(st==3)written=snprintf(out,cap,"{\"state\":\"QUEUED\",\"request_id\":\"%llu\",\"marker_id\":\"%llu\"}",(unsigned long long)request,(unsigned long long)n->marker.id);
 else written=snprintf(out,cap,"{\"state\":\"PENDING\",\"request_id\":\"%llu\"}",(unsigned long long)request);
 if(written<0||(size_t)written>=cap)return 503;
 return st==4?409:st==3?200:202;
}
int network_extra_request(NetworkExtra*n,const char*m,const char*p,const char*b,size_t bytes,char*out,size_t cap){
 if(cap)out[0]=0;
 bool marker=!strcmp(p,"/api/marker")||!strncmp(p,"/api/marker/",12);
 bool report=!strcmp(p,"/api/report")||!strncmp(p,"/api/report/",12);
 if(!marker&&!report)return 404;
 if(!take(n))return 503;
 int result=400;uint64_t id=0;
 if(!strcmp(m,"GET")&&strlen(p)>12){
  if(!token(p+12,&id))goto done;
  if(id!=(marker?n->marker.request:n->report.request)){result=404;goto done;}
  result=status(n,marker,out,cap);goto done;
 }
 if(strcmp(m,"POST")||strlen(p)!=11){result=405;goto done;}
 if(!b||!metadata_json_parse(&json,(const uint8_t*)b,bytes)||json.token[0].type!=MJ_OBJECT||
    !metadata_json_u64(&json,metadata_json_field(&json,0,"request_id"),&id)||!id)goto done;
 if(marker){
  uint32_t strength;char note[513];
  int note_token=metadata_json_field(&json,0,"note");note[0]=0;
  if(!metadata_json_u32(&json,metadata_json_field(&json,0,"strength"),&strength)||strength<1||strength>5||(note_token>=0&&!storage_marker_note_decode(&json,note_token,note)))goto done;
  int state=atomic_load_explicit(&n->marker.state,memory_order_acquire);
  if(id==n->marker.request){result=(strength==n->marker.strength&&!strcmp(note,n->marker.note))?status(n,true,out,cap):409;goto done;}
  if(state==1||state==2){result=409;goto done;}
  n->marker.request=id;n->marker.strength=strength;strcpy(n->marker.note,note);n->marker.id=0;n->marker.error=0;
  atomic_store_explicit(&n->marker.state,1,memory_order_release);
 }else{
  char path[64],compare[64];
  if(!text_field("path",path,sizeof path)||!text_field("compare_path",compare,sizeof compare)||!report_path(path)||!report_path(compare))goto done;
  int state=atomic_load_explicit(&n->report.state,memory_order_acquire);
  if(id==n->report.request){result=(!strcmp(path,n->report.path)&&!strcmp(compare,n->report.compare))?status(n,false,out,cap):409;goto done;}
  if(state==1||state==2){result=409;goto done;}
  n->report.request=id;strcpy(n->report.path,path);strcpy(n->report.compare,compare);n->report.error=0;n->report.data[0]=0;
  atomic_store_explicit(&n->report.state,1,memory_order_release);
 }
 result=status(n,marker,out,cap);
done:if(cap&&!out[0])snprintf(out,cap,"{\"error\":\"%s\"}",result==409?"REQUEST_CONFLICT":result==404?"NOT_FOUND":result==405?"METHOD_NOT_ALLOWED":result==503?"RESPONSE_CAPACITY":"INVALID_REQUEST");give(n);return result;
}
void network_extra_capture_step(NetworkExtra*n){
 if(!take(n))return;
 if(atomic_load(&n->marker.state)!=1){give(n);return;}
 atomic_store(&n->marker.state,2);give(n);
 int error=n->hooks.marker?n->hooks.marker(n->hooks.context,n->marker.strength,n->marker.note,strlen(n->marker.note),&n->marker.id):1;
 n->marker.error=error;atomic_store_explicit(&n->marker.state,error?4:3,memory_order_release);
}
void network_extra_io_step(NetworkExtra*n){
 if(!take(n))return;
 if(atomic_load(&n->report.state)!=1){give(n);return;}
 atomic_store(&n->report.state,2);give(n);
 int error=n->hooks.report?n->hooks.report(n->hooks.context,n->report.path,n->report.compare,n->report.data,sizeof n->report.data):1;
 n->report.error=error;atomic_store_explicit(&n->report.state,error?4:3,memory_order_release);
}
bool network_extra_busy(NetworkExtra*n){int st=atomic_load_explicit(&n->report.state,memory_order_acquire);return st==1||st==2;}
