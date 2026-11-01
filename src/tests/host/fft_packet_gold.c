/* Independent shared wire codec, using official C-model integers as payload. */
#include "protocol.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int number(const char *text,unsigned long long *out) {
 char *end;errno=0;*out=strtoull(text,&end,10);
 return text[0]>='0'&&text[0]<='9'&&!errno&&!*end;
}
int main(int argc,char **argv) {
 if(argc!=7)return 2;
 unsigned long long seq,raw,flags,exp;
 if(!number(argv[3],&seq)||!number(argv[4],&raw)||!number(argv[5],&flags)||
    !number(argv[6],&exp)||flags>63||exp>12)return 2;
 int32_t values[8192];uint8_t bytes[LFN_MAX_PACKET];size_t written=0;
 FILE *file=fopen(argv[1],"rb");if(!file)return 3;
 size_t count=fread(values,sizeof(values[0]),8192,file);int extra=fgetc(file);
 int closed=fclose(file);if(count!=8192||extra!=EOF||closed)return 4;
 PacketHeader header={LFN_FFT,(uint16_t)flags,1,seq,raw,1,(uint8_t)exp};
 if(!packet_encode(&header,values,8192,bytes,sizeof(bytes),&written))return 5;
 PacketView decoded;
 if(!packet_decode(bytes,written,&decoded))return 6;
 for(size_t i=0;i<8192;i++){int32_t value;if(!packet_value(&decoded,i,&value)||value!=values[i])return 7;}
 file=fopen(argv[2],"wb");if(!file)return 8;
 count=fwrite(bytes,1,written,file);closed=fclose(file);
 if(count!=written||closed)return 9;
 printf("PASS fft_packet_gold\n");return 0;
}
