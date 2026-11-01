#include "network_capture.h"
#include <stdio.h>
#include <string.h>
void network_capture_init(NetworkCapture*c,const NetworkCaptureHooks*h){memset(c,0,sizeof(*c));c->hooks=*h;}
int network_capture_start(NetworkCapture*c,uint64_t session,uint64_t now){
 if(!session)return 400;
 if(c->session==session)return 200;
 if(c->phase!=NET_CAPTURE_IDLE&&c->phase!=NET_CAPTURE_STOPPED)return 409;
 c->session=session;c->since=now;c->phase=NET_CAPTURE_BEGIN;c->stop_requested=c->stop_sent=false;c->owns_start=true;c->failure=0;return 202;
}
int network_capture_stop(NetworkCapture*c,uint64_t now){
 if(c->phase==NET_CAPTURE_IDLE||c->phase==NET_CAPTURE_STOPPED||c->stop_requested)return 200;
 c->stop_requested=true;c->since=now;return 202;
}
static void fail(NetworkCapture*c,int32_t code){c->phase=NET_CAPTURE_ERROR;if(!c->failure)c->failure=code?code:-1000;}
void network_capture_step(NetworkCapture*c,const NetworkCaptureObservation*o,uint64_t now){
 c->observed=*o;
 if((c->phase==NET_CAPTURE_IDLE||(c->phase==NET_CAPTURE_STOPPED&&c->session!=o->controller.session_id))&&o->have_controller&&o->controller.start_command){
  c->session=o->controller.session_id;c->since=now;c->owns_start=false;c->stop_requested=c->stop_sent=false;
  if(o->controller.state==PRE_ADC_RUNNING)c->phase=NET_CAPTURE_RUNNING;
  else if(o->controller.state==PRE_ADC_STARTING)c->phase=NET_CAPTURE_START;
  else if(o->controller.state==PRE_ADC_STOPPING){c->phase=NET_CAPTURE_STOP;c->stop_requested=c->stop_sent=true;}
  else if(o->controller.state==PRE_ADC_STOPPED&&o->storage_available&&o->storage_closed&&!o->storage_active&&!o->storage_failed)c->phase=NET_CAPTURE_STOPPED;
 }
 if((o->have_controller&&(o->controller.fault_latched||o->controller.failure||o->controller.state==PRE_ADC_FAULT))||
    (o->storage_available&&o->storage_failed))fail(c,o->controller.failure?o->controller.failure:-1002);
 if(c->phase==NET_CAPTURE_BEGIN){
  /* The previous normal STOP can finish detach a few owner turns later. */
  if(c->hooks.can_begin(c->hooks.context,c->session)){
   if(!c->hooks.storage_begin(c->hooks.context,c->session)){fail(c,-1003);return;}
   c->phase=NET_CAPTURE_STORAGE;
  }
 }else if(c->phase==NET_CAPTURE_STORAGE){
  if(o->storage_available&&o->storage_ready&&o->storage_session==c->session){
   if(!c->hooks.begin(c->hooks.context,c->session)||!c->hooks.start(c->hooks.context)){fail(c,-1004);return;}
   c->phase=NET_CAPTURE_START;
  }
 }else if(c->phase==NET_CAPTURE_START){
  if(o->have_controller&&o->controller.session_id==c->session){
   if(o->controller.start==PRE_ADC_REJECTED||o->controller.start==PRE_ADC_FAILED){fail(c,-1005);return;}
   if(o->controller.state==PRE_ADC_RUNNING&&o->controller.start==PRE_ADC_RUN_CONFIRMED){
    if((c->owns_start&&(o->source!=PL_CAPTURE_ADC||o->channel!=0))||!o->controller.capture_id||
       !o->storage_available||!o->storage_active||o->storage_session!=c->session){fail(c,-1006);return;}
    c->phase=NET_CAPTURE_RUNNING;
   }
  }
 }
 /* A cold START cancelled before facts exist must first become RUN, so the
  * existing storage owner can seal a real capture instead of invented facts. */
 if(c->stop_requested&&!c->stop_sent&&(c->phase==NET_CAPTURE_RUNNING||c->phase==NET_CAPTURE_ERROR)){
  if(c->hooks.stop(c->hooks.context)){c->stop_sent=true;if(c->phase!=NET_CAPTURE_ERROR)c->phase=NET_CAPTURE_STOP;}
 }
 if(c->phase==NET_CAPTURE_RUNNING&&o->have_controller&&o->controller.session_id==c->session&&o->controller.state==PRE_ADC_STOPPING){
  c->phase=NET_CAPTURE_STOP;c->stop_requested=c->stop_sent=true;
 }
 if((c->phase==NET_CAPTURE_STOP||c->phase==NET_CAPTURE_RUNNING)&&o->have_controller&&o->controller.session_id==c->session&&
    o->controller.state==PRE_ADC_STOPPED&&o->controller.stop==PRE_ADC_STOP_CONFIRMED&&
    o->storage_available&&!o->storage_active&&o->storage_closed&&!o->storage_failed)c->phase=NET_CAPTURE_STOPPED;
 if(c->phase>=NET_CAPTURE_BEGIN&&c->phase<=NET_CAPTURE_START&&(now<c->since||now-c->since>UINT64_C(90000000)))fail(c,-1007);
}
bool network_capture_json(const NetworkCapture*c,char*out,size_t cap){
 static const char*names[]={"idle","starting","starting","starting","running","stopping","stopped","error"};
 const NetworkCaptureObservation*o=&c->observed;
 bool active=c->phase!=NET_CAPTURE_IDLE&&c->phase!=NET_CAPTURE_STOPPED;
 const char*phase=names[c->phase];if(c->stop_requested&&c->phase!=NET_CAPTURE_ERROR&&c->phase!=NET_CAPTURE_STOPPED)phase="stopping";
 const char*source=o->have_controller&&o->controller.start_command?(o->source==PL_CAPTURE_ADC?"\"ADC\"":"\"TEST\""):(c->session?"\"ADC\"":"null");
 int n=snprintf(out,cap,"{\"phase\":\"%s\",\"session\":\"%llu\",\"source\":%s,\"channel\":%u,\"config_id\":%u,\"capture_id\":\"%llu\",\"failure\":%ld,\"may_be_running\":%s,\"storage\":{\"available\":%s,\"active\":%s,\"closed\":%s,\"failed\":%s}}",
  phase,(unsigned long long)c->session,source,(unsigned)o->channel,(unsigned)o->controller.config_id,
  (unsigned long long)o->controller.capture_id,(long)c->failure,active?"true":"false",o->storage_available?"true":"false",
  o->storage_active?"true":"false",o->storage_closed?"true":"false",o->storage_failed?"true":"false");
 return n>=0&&(size_t)n<cap;
}
