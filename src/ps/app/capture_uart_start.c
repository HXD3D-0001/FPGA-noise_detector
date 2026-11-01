#include "capture_uart_start.h"
#include "raw_capture.h"
#include <string.h>
bool capture_uart_parse_start(const char *line,CaptureUartStart *out){
 if(!line||!out)return false;
 bool adc=!strncmp(line,"STARTADC ",9);
 if(!adc&&strncmp(line,"START ",6))return false;
 const char*p=line+(adc?9:6);uint64_t values[6]={0};unsigned n=0;
 while(*p){
  while(*p==' '||*p=='\t')p++;
  if(!*p)break;
  if(n==6||*p<'0'||*p>'9')return false;
  uint64_t value=0;
  do{unsigned digit=(unsigned)(*p-'0');if(value>(UINT64_MAX-digit)/10)return false;value=value*10+digit;p++;}while(*p>='0'&&*p<='9');
  if(*p&&*p!=' '&&*p!='\t')return false;
  values[n++]=value;
 }
 unsigned base=adc?3u:4u;
 if(n!=base&&n!=base+2)return false;
 if(!values[0]||values[0]>UINT32_MAX||values[1]>2||values[2]>1||(!adc&&values[3]>3))return false;
 uint64_t duration=n>base?values[base]:0,capacity=n>base?values[base+1]:0;
 if(!values[1]){if(duration||capacity)return false;}
 else if(!duration||capacity<LFN_RAW_PACKET_BYTES||capacity>LFN_RAW_BUFFER_BYTES)return false;
 CaptureUartStart result={0};
 result.config.config_id=(uint32_t)values[0];result.config.mode=(uint32_t)values[1];result.config.channel=(uint32_t)values[2];
 result.config.source=adc?PL_CAPTURE_ADC:PL_CAPTURE_TEST;result.config.pattern=adc?0:(uint32_t)values[3];
 result.raw_duration=duration;result.raw_capacity=capacity;*out=result;return true;
}
