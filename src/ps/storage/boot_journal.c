#include "boot_journal.h"
#include "protocol.h"
#include <string.h>
static void put(uint8_t *b,uint64_t x,unsigned n){for(unsigned i=0;i<n;i++)b[i]=(uint8_t)(x>>(8*i));}
static uint64_t get(const uint8_t *b,unsigned n){uint64_t x=0;for(unsigned i=0;i<n;i++)x|=(uint64_t)b[i]<<(8*i);return x;}
static void fail(BootJournal *j,StorageError e){j->state=BOOT_FAULT;j->error=e;}
static bool io(BootJournal *j,StorageIoResult r){if(r==STORE_IO_OK)return true;fail(j,r==STORE_IO_NO_MEDIA?STORE_ERROR_NO_MEDIA:r==STORE_IO_NO_SPACE?STORE_ERROR_NO_SPACE:STORE_ERROR_IO);return false;}
bool boot_journal_init(BootJournal *j,const StorageBackend copies[2],const char *device,size_t n,bool provision){
 if(!j||!copies||!device||!n||n>=64)return false;
 for(size_t i=0;i<n;i++){unsigned char c=(unsigned char)device[i];if(c<33||c>126||c=='"'||c=='\\')return false;}
 for(unsigned i=0;i<2;i++)if(!copies[i].size||!copies[i].read||!copies[i].write||!copies[i].sync)return false;
 if(copies[0].context==copies[1].context&&copies[0].write==copies[1].write)return false;
 StorageBackend b[2]={copies[0],copies[1]};char id[64]={0};memcpy(id,device,n);memset(j,0,sizeof(*j));memcpy(j->device_id,id,64);memcpy(j->copies,b,sizeof(b));j->provision=provision;j->state=BOOT_SIZE_0;return true;
}
static void decode(BootJournal *j,unsigned k){
 const uint8_t *b=j->frame;
 if(memcmp(b,"BTJ1",4)||get(b+4,2)!=1||get(b+6,2)||get(b+252,4)!=crc32c(b,252)||!get(b+8,8))return;
 for(unsigned i=80;i<252;i++)if(b[i])return;
 if(memcmp(b+16,j->device_id,64)){fail(j,STORE_ERROR_DEPENDENCY);return;}
 j->valid[k]=true;j->sequence[k]=get(b+8,8);
}
void boot_journal_step(BootJournal *j){
 if(!j)return;
 size_t actual=0;
 switch(j->state){
 case BOOT_SIZE_0:case BOOT_SIZE_1:{
  unsigned k=j->state==BOOT_SIZE_0?0:1;uint64_t n=0;if(!io(j,j->copies[k].size(j->copies[k].context,&n)))return;
  if(n>BOOT_JOURNAL_BYTES){fail(j,STORE_ERROR_DEPENDENCY);return;}
  j->empty[k]=n==0;
  if(n==BOOT_JOURNAL_BYTES)j->state=k?BOOT_READ_1:BOOT_READ_0;
  else j->state=k?BOOT_CHOOSE:BOOT_SIZE_1;
  break;}
 case BOOT_READ_0:case BOOT_READ_1:{
  unsigned k=j->state==BOOT_READ_0?0:1;
  if(!io(j,j->copies[k].read(j->copies[k].context,0,j->frame,256,&actual)))return;
  if(actual!=256){fail(j,STORE_ERROR_SHORT_READ);return;}decode(j,k);if(j->state==BOOT_FAULT)return;j->state=k?BOOT_CHOOSE:BOOT_SIZE_1;break;}
 case BOOT_CHOOSE:{
  uint64_t greatest=0;
  if(!j->valid[0]&&!j->valid[1]){if(!j->provision||!j->empty[0]||!j->empty[1]){fail(j,STORE_ERROR_DEPENDENCY);return;}j->target=0;}
  else {
   if(j->valid[0]&&j->valid[1]){uint64_t a=j->sequence[0],b=j->sequence[1];if((a>b?a-b:b-a)>1){fail(j,STORE_ERROR_DEPENDENCY);return;}}
   greatest=j->sequence[0]>j->sequence[1]?j->sequence[0]:j->sequence[1];j->target=!j->valid[0]?0:!j->valid[1]?1:j->sequence[0]<=j->sequence[1]?0:1;
  }
  if(greatest==UINT64_MAX){fail(j,STORE_ERROR_DEPENDENCY);return;}j->session_id=greatest+1;
  memset(j->frame,0,256);memcpy(j->frame,"BTJ1",4);put(j->frame+4,1,2);put(j->frame+8,j->session_id,8);memcpy(j->frame+16,j->device_id,64);put(j->frame+252,crc32c(j->frame,252),4);j->state=BOOT_WRITE;break;}
 case BOOT_WRITE:
  if(!io(j,j->copies[j->target].write(j->copies[j->target].context,0,j->frame,256,&actual)))return;
  if(actual!=256){fail(j,STORE_ERROR_SHORT_WRITE);return;}j->state=BOOT_SYNC;break;
 case BOOT_SYNC:
  if(!io(j,j->copies[j->target].sync(j->copies[j->target].context)))return;
  if(!j->completed){j->completed=1;j->target^=1;j->state=BOOT_WRITE;}else j->state=BOOT_READY;
  break;
 case BOOT_READY:case BOOT_FAULT:break;
 }
}
bool boot_journal_session(const BootJournal *j,uint64_t *out){if(!j||!out||j->state!=BOOT_READY)return false;*out=j->session_id;return true;}
