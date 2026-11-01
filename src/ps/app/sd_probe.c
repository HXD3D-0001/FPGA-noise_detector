#include "sd_probe.h"
#include "../storage/fatfs_backend.h"
#include "../storage/metadata_json.h"
#include "../storage/storage_service.h"
#include "ff.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
enum { SD_PROBE_ENTRIES=32, SD_PROBE_SCAN_LIMIT=256, SD_PROBE_LINE=192, SD_WRITE_BYTES=4096, SD_WRITE_ENTRIES=64 };
static LfnFatFsVolume volume;
static LfnFatFsScan scan;
static LfnFatFsEntry entry;
static LfnFatFsFile file;
static StorageBackend backend;
static uint8_t payload[SD_WRITE_BYTES],readback[SD_WRITE_BYTES];
static TaskHandle_t probe_owner;
static bool owner_ok(void *v){ (void)v; return probe_owner && xTaskGetCurrentTaskHandle()==probe_owner; }
static bool (*vol_owner_ok)(void *);
enum { SDX_HEADER=48,SDX_BLOCK=4096,SDX_IDLE_MS=15000 };
static struct {
 bool active,mounted,opened,failed,cached;
 uint64_t nonce,next,total,last;
 uint32_t seq,crc,touched;unsigned retries;
 size_t cached_bytes;
 uint8_t frame[SDX_HEADER+SDX_BLOCK];
} sdx;
bool lfn_sd_export_busy(void){return sdx.active;}
static uint32_t sdx_crc(uint32_t crc,const uint8_t *b,size_t n){
 for(size_t i=0;i<n;i++){crc^=b[i];for(unsigned j=0;j<8;j++)crc=(crc>>1)^((crc&1u)?0x82F63B78u:0);}
 return crc;
}
static void sdx_le(uint8_t *p,uint64_t n,unsigned bytes){for(unsigned i=0;i<bytes;i++){p[i]=(uint8_t)n;n>>=8;}}
static void sdx_header(uint8_t *b,unsigned type,unsigned status,uint64_t offset,size_t n){
 memcpy(b,"SDX1",4);b[4]=1;b[5]=(uint8_t)type;sdx_le(b+6,status,2);
 sdx_le(b+8,sdx.nonce,8);sdx_le(b+16,sdx.seq,4);sdx_le(b+20,offset,8);
 sdx_le(b+28,sdx.total,8);sdx_le(b+36,n,4);sdx_le(b+40,sdx.crc^UINT32_MAX,4);
 uint32_t crc=sdx_crc(UINT32_MAX,b,44);crc=sdx_crc(crc,b+48,n);
 sdx_le(b+44,crc^UINT32_MAX,4);
}
static bool sdx_clean(void){
 bool ok=true;
 if(sdx.opened){if(lfn_fatfs_close(&file)==STORE_IO_OK)sdx.opened=false;else ok=false;}
 if(sdx.mounted){if(lfn_fatfs_unmount(&volume)==STORE_IO_OK)sdx.mounted=false;else ok=false;}
 sdx.active=sdx.opened||sdx.mounted;sdx.failed=sdx.active;return ok;
}
static bool sdx_reply(LfnSdExportSend send,unsigned type,unsigned status){
 uint8_t h[SDX_HEADER];sdx_header(h,type,status,sdx.next,0);
 if(send(h,sizeof h))return true;
 (void)sdx_clean();return false;
}
static void sdx_error(LfnSdExportSend send,unsigned status,bool abort){
 if(abort&&!sdx_clean())status=8;
 (void)sdx_reply(send,5,status);
}
/* Strict decimal tokens: reject signs, overflow and trailing characters. */
static bool sdx_number(const char **p,uint64_t *value){
 const char *s=*p;uint64_t n=0;if(*s<'0'||*s>'9')return false;
 do{unsigned d=(unsigned)(*s-'0');if(n>(UINT64_MAX-d)/10)return false;n=n*10+d;s++;}while(*s>='0'&&*s<='9');
 *p=s;*value=n;return true;
}
bool lfn_sd_export_command(void *owner_task,LfnSdExportSend send,const char *line,uint32_t now_ms){
 if(!owner_task||!send||!line)return false;
 if(strncmp(line,"SDX",3)){if(!sdx.active)return false;sdx_error(send,2,false);return true;}
 if(sdx.active&&probe_owner!=(TaskHandle_t)owner_task)return true;
 sdx.touched=now_ms;
 if(!strcmp(line,"SDXCANCEL")){
  bool ok=sdx_clean();(void)sdx_reply(send,4,ok?0:8);return true;
 }
 if(!strncmp(line,"SDXOPEN ",8)){
  if(sdx.active){sdx_error(send,2,false);return true;}
  const char *p=line+8;uint64_t nonce;
  if(!sdx_number(&p,&nonce)||!nonce||*p++!=' '||!*p||strlen(p)>160||strchr(p,' ')||strchr(p,'\t')){
   sdx_error(send,1,false);return true;
  }
  memset(&sdx,0,sizeof sdx);sdx.nonce=nonce;sdx.crc=UINT32_MAX;sdx.touched=now_ms;
  probe_owner=(TaskHandle_t)owner_task;
  if(!lfn_fatfs_volume_init(&volume,"0:",owner_ok,NULL)||lfn_fatfs_mount(&volume)!=STORE_IO_OK){sdx_error(send,3,true);return true;}
  sdx.active=sdx.mounted=true;
  if(lfn_fatfs_open(&file,&volume,p,false,false)!=STORE_IO_OK){sdx_error(send,4,true);return true;}
  sdx.opened=true;
  if(!lfn_fatfs_backend(&file,&backend)||!backend.size||!backend.read||backend.size(backend.context,&sdx.total)!=STORE_IO_OK){sdx_error(send,5,true);return true;}
  (void)sdx_reply(send,1,0);return true;
 }
 bool read=!strncmp(line,"SDXREAD ",8),end=!strncmp(line,"SDXEND ",7);
 const char *p=line+(read?8:end?7:0);uint64_t nonce,offset=0;
 if((!read&&!end)||!sdx_number(&p,&nonce)||(read&&(*p++!=' '||!sdx_number(&p,&offset)))||*p){sdx_error(send,1,false);return true;}
 if(sdx.failed){sdx_error(send,8,false);return true;}
 if(!sdx.active||nonce!=sdx.nonce){sdx_error(send,2,false);return true;}
 if(end){
  uint64_t total=0;
  if(sdx.next!=sdx.total||backend.size(backend.context,&total)!=STORE_IO_OK||total!=sdx.total){sdx_error(send,6,true);return true;}
  bool ok=sdx_clean();(void)sdx_reply(send,3,ok?0:8);return true;
 }
 if(sdx.cached&&offset==sdx.last){
  if(++sdx.retries>3){sdx_error(send,7,true);return true;}
  if(!send(sdx.frame,sdx.cached_bytes))(void)sdx_clean();
  return true;
 }
 if(offset!=sdx.next||offset>=sdx.total||sdx.seq==UINT32_MAX){sdx_error(send,6,false);return true;}
 size_t n=sdx.total-offset>SDX_BLOCK?SDX_BLOCK:(size_t)(sdx.total-offset),got=0;
 if(backend.read(backend.context,offset,sdx.frame+SDX_HEADER,n,&got)!=STORE_IO_OK||got!=n){sdx_error(send,5,true);return true;}
 sdx.last=offset;sdx.next+=n;sdx.seq++;sdx.retries=0;sdx.cached=true;
 sdx.crc=sdx_crc(sdx.crc,sdx.frame+SDX_HEADER,n);
 sdx_header(sdx.frame,2,0,offset,n);sdx.cached_bytes=SDX_HEADER+n;
 if(!send(sdx.frame,sdx.cached_bytes))(void)sdx_clean();
 return true;
}
void lfn_sd_export_poll(LfnSdExportSend send,uint32_t now_ms){
 if(sdx.active&&!sdx.failed&&(uint32_t)(now_ms-sdx.touched)>=SDX_IDLE_MS)sdx_error(send,9,true);
}
static bool shared_owner_ok(void *v){return vol_owner_ok&&vol_owner_ok(v);}
bool (*lfn_sd_probe_owner_ok(void *owner_task))(void *){if(sdx.active)return NULL;probe_owner=(TaskHandle_t)owner_task;vol_owner_ok=owner_ok;return shared_owner_ok;}
static void say(void (*emit)(const char *), const char *fmt, ...){
 char line[SD_PROBE_LINE];
 va_list ap; va_start(ap,fmt);
 int n=vsnprintf(line,sizeof(line),fmt,ap); va_end(ap);
 if(n>0 && n<(int)sizeof(line)) (void)emit(line);
}
typedef struct {uint64_t count,first,end;} SdVerifyRange;
typedef struct {
 uint64_t bytes,prefix,records,first_seq,last_seq;
 uint64_t faults,delivery,ingress,ingress_overflow,other_faults,route_lock,route_full,route_unknown;
 SdVerifyRange data[4];
} SdVerifyStats;
/* Single probe-task owner; the maximum frame and JSON tokens must not occupy
 * the small FreeRTOS task stack. No writes or repair callbacks are used. */
static uint8_t verify_frame[RECORD_MAX_BYTES];
static MetadataJson verify_json;
static bool verify_text(const char *key,const char *value){
 int t=metadata_json_field(&verify_json,0,key);
 return t>=0&&verify_json.token[t].type==MJ_STRING&&metadata_json_is(&verify_json,t,value);
}
static void verify_fault(const RecordView *v,SdVerifyStats *s){
 /* Counts cover every framed FAULT in this file, not just the requested
  * capture. Unknown payloads remain diagnostic "other", never scan errors. */
 s->faults++;
 if(!metadata_json_parse(&verify_json,v->payload,v->payload_bytes)){
  s->other_faults++;return;
 }
 if(verify_text("component","service")&&verify_text("reason","DELIVERY_LOSS")){
  s->delivery++;return;
 }
 if(!verify_text("component","capture_route")){s->other_faults++;return;}
 if(verify_text("reason","INGRESS_LOSS"))s->ingress++;
 else if(verify_text("reason","INGRESS_OVERFLOW"))s->ingress_overflow++;
 else{s->other_faults++;return;}
 uint32_t reason;
 if(!metadata_json_u32(&verify_json,metadata_json_field(&verify_json,0,"route_reason"),&reason)||
    !reason||(reason&~(STORAGE_INGRESS_FULL|STORAGE_INGRESS_LOCK|STORAGE_INGRESS_OVERFLOW))){
  s->route_unknown++;return;
 }
 if(reason&STORAGE_INGRESS_LOCK)s->route_lock++;
 if(reason&STORAGE_INGRESS_FULL)s->route_full++;
}
static bool verify_read(const StorageBackend *b,uint64_t off,uint8_t *dst,size_t n){
 while(n){
  size_t chunk=n>STORAGE_IO_CHUNK?STORAGE_IO_CHUNK:n,got=0;
  if(b->read(b->context,off,dst,chunk,&got)!=STORE_IO_OK||got!=chunk)return false;
  off+=chunk;dst+=chunk;n-=chunk;
 }
 return true;
}
static const char *verify_scan(const StorageBackend *b,uint64_t session,
 uint64_t capture,uint32_t config,SdVerifyStats *s){
 memset(s,0,sizeof(*s));
 if(!b->size||!b->read||b->size(b->context,&s->bytes)!=STORE_IO_OK)return "size";
 bool have_mode=false;uint32_t mode=0;
 while(s->prefix<s->bytes){
  uint64_t remaining=s->bytes-s->prefix;
  if(remaining<RECORD_HEADER_BYTES)return "partial_header";
  if(!verify_read(b,s->prefix,verify_frame,RECORD_HEADER_BYTES))return "read_header";
  uint32_t payload_bytes=(uint32_t)verify_frame[12]|((uint32_t)verify_frame[13]<<8)|
   ((uint32_t)verify_frame[14]<<16)|((uint32_t)verify_frame[15]<<24);
  if(payload_bytes>RECORD_MAX_BYTES-RECORD_HEADER_BYTES-RECORD_TAIL_BYTES)return "frame_size";
  size_t total=RECORD_HEADER_BYTES+(size_t)payload_bytes+RECORD_TAIL_BYTES;
  if(total>remaining)return "partial_frame";
  if(!verify_read(b,s->prefix+RECORD_HEADER_BYTES,verify_frame+RECORD_HEADER_BYTES,
    total-RECORD_HEADER_BYTES))return "read_body";
  RecordView v;
  if(!record_decode(verify_frame,total,&v)||v.total_bytes!=total)return "frame_crc";
  const RecordIdentity *id=&v.identity;
  if(id->session_id!=session)return "session";
  if(!id->record_seq||id->record_seq<=s->last_seq)return "sequence";
  if(id->type==RECORD_FAULT)verify_fault(&v,s);
  if(id->type==RECORD_CONFIG&&id->config_id==config){
   uint32_t next_mode;
   if(!metadata_json_parse(&verify_json,v.payload,v.payload_bytes)||
      !metadata_json_u32(&verify_json,metadata_json_field(&verify_json,0,"mode"),&next_mode)||
      next_mode>2||(have_mode&&next_mode!=mode))return "config_mode";
   mode=next_mode;have_mode=true;
  }
  if(id->capture_id==capture&&id->config_id==config){
   int kind=-1;
   if(id->type==RECORD_ENERGY)kind=0;
   else if(id->type==RECORD_PSD)kind=2;
   else if(id->type==RECORD_PCM){
    if(!have_mode)return "missing_mode";
    kind=mode?3:1;
   }
   if(kind>=0){
    if(id->first_raw==id->end_raw)return "empty_range";
    SdVerifyRange *r=&s->data[kind];
    if(!r->count||id->first_raw<r->first)r->first=id->first_raw;
    if(!r->count||id->end_raw>r->end)r->end=id->end_raw;
    r->count++;
   }
  }
  if(!s->records)s->first_seq=id->record_seq;
  s->last_seq=id->record_seq;s->records++;s->prefix+=total;
 }
 return s->data[0].count||s->data[1].count||s->data[2].count||s->data[3].count?NULL:"no_target_data";
}
bool lfn_sd_verify_run(void *owner_task,void (*emit)(const char *),
 uint64_t session,uint64_t capture,uint32_t config,const char *relative_path){
 if(sdx.active)return false;
 if(!owner_task||!emit||!session||!capture||!config||!relative_path||!*relative_path)return false;
 probe_owner=(TaskHandle_t)owner_task;
 SdVerifyStats stats={0};const char *reason="volume_init";
 bool mounted=false,opened=false;
 StorageIoResult close_result=STORE_IO_OK,unmount_result=STORE_IO_OK;
 say(emit,"LFN SDV TARGET session=%llu capture=%llu config=%lu\r\n",
  (unsigned long long)session,(unsigned long long)capture,(unsigned long)config);
 if(!lfn_fatfs_volume_init(&volume,"0:",owner_ok,NULL))goto done;
 reason="mount";
 if(lfn_fatfs_mount(&volume)!=STORE_IO_OK)goto done;
 mounted=true;reason="open";
 if(lfn_fatfs_open(&file,&volume,relative_path,false,false)!=STORE_IO_OK)goto done;
 opened=true;reason="backend";
 if(!lfn_fatfs_backend(&file,&backend))goto done;
 reason=verify_scan(&backend,session,capture,config,&stats);
done:
 if(opened)close_result=lfn_fatfs_close(&file);
 if(mounted)unmount_result=lfn_fatfs_unmount(&volume);
 say(emit,"LFN SDV TOTAL ok=%u records=%llu bytes=%llu prefix=%llu\r\n",reason?0u:1u,
  (unsigned long long)stats.records,(unsigned long long)stats.bytes,(unsigned long long)stats.prefix);
 say(emit,"LFN SDV ORDER first_seq=%llu last_seq=%llu\r\n",
  (unsigned long long)stats.first_seq,(unsigned long long)stats.last_seq);
 const char *names[]={"energy","pcm","psd","raw"};
 for(unsigned k=0;k<4;k++)say(emit,"LFN SDV MATCH kind=%s count=%llu first=%llu end=%llu\r\n",names[k],
  (unsigned long long)stats.data[k].count,(unsigned long long)stats.data[k].first,(unsigned long long)stats.data[k].end);
 say(emit,"LFN SDV FAULT total=%llu delivery=%llu ingress=%llu\r\n",
  (unsigned long long)stats.faults,(unsigned long long)stats.delivery,(unsigned long long)stats.ingress);
 say(emit,"LFN SDV FAULT_EXTRA overflow=%llu other=%llu\r\n",
  (unsigned long long)stats.ingress_overflow,(unsigned long long)stats.other_faults);
 say(emit,"LFN SDV ROUTE lock=%llu full=%llu unknown=%llu\r\n",
  (unsigned long long)stats.route_lock,(unsigned long long)stats.route_full,(unsigned long long)stats.route_unknown);
 say(emit,"LFN SDV FAULT_SCOPE whole_file_record_counts_partial_if_scan_failed\r\n");
 say(emit,"LFN SDV SCOPE frame_crc_session_sequence_identity_ranges\r\n");
 if(!reason&&close_result!=STORE_IO_OK)reason="close";
 if(!reason&&unmount_result!=STORE_IO_OK)reason="unmount";
 say(emit,"LFN SDV DONE result=%s reason=%s close=%d unmount=%d\r\n",
  reason?"fail":"ok",reason?reason:"none",(int)close_result,(int)unmount_result);
 return reason==NULL;
}
static const char *type_name(unsigned t){
 switch(t){case 1:return "FAT12";case 2:return "FAT16";case 3:return "FAT32";case 4:return "EXFAT";}
 return "UNKNOWN";
}
bool lfn_sd_probe_run(void *owner_task, void (*emit)(const char *line)){
 if(sdx.active)return false;
 if(!owner_task||!emit)return false;
 probe_owner=(TaskHandle_t)owner_task;
 bool ok=false;
 if(!lfn_fatfs_volume_init(&volume,"0:",owner_ok,NULL)){
  say(emit,"LFN SD MOUNT ok=0 reason=volume_init\r\n");
  say(emit,"LFN SD DONE result=fail\r\n");
  return false;
 }
 StorageIoResult mount=lfn_fatfs_mount(&volume);
 say(emit,"LFN SD MOUNT ok=%d result=%d\r\n",mount==STORE_IO_OK?1:0,(int)mount);
 if(mount!=STORE_IO_OK){
  say(emit,"LFN SD DONE result=fail\r\n");
  return false;
 }
 unsigned type=0;
 bool have_type=lfn_fatfs_type(&volume,&type);
 uint64_t free_bytes=0,total_bytes=0;
 bool have_space=lfn_fatfs_space(&volume,&free_bytes,&total_bytes);
 say(emit,"LFN SD TYPE %s (fs_type=%u known=%d)\r\n",have_type?type_name(type):"UNKNOWN",type,have_type?1:0);
 say(emit,"LFN SD SPACE free=%llu total=%llu known=%d\r\n",
     (unsigned long long)(have_space?free_bytes:0),(unsigned long long)(have_space?total_bytes:0),have_space?1:0);
 unsigned entries=0,listed=0; bool scan_ok=false;
 if(lfn_fatfs_scan_open(&scan,&volume,"")==STORE_IO_OK){
  scan_ok=true;
  for(unsigned i=0;i<SD_PROBE_SCAN_LIMIT;i++){
   bool have=false;
   if(lfn_fatfs_scan_next(&scan,&entry,&have)!=STORE_IO_OK){scan_ok=false;break;}
   if(!have)break;
   entries++;
   if(listed<SD_PROBE_ENTRIES && entry.name[0]){
    say(emit,"LFN SD ENTRY name=%s bytes=%llu dir=%u\r\n",entry.name,
        (unsigned long long)entry.bytes,entry.directory?1u:0u);
    listed++;
   }
  }
  (void)lfn_fatfs_scan_close(&scan);
 }
 say(emit,"LFN SD SUMMARY entries=%u listed=%u scan=%s\r\n",entries,listed,scan_ok?"OK":"FAIL");
 StorageIoResult unmount=lfn_fatfs_unmount(&volume);
 say(emit,"LFN SD UNMOUNT result=%d\r\n",(int)unmount);
 ok=(unmount==STORE_IO_OK);
 say(emit,"LFN SD DONE result=%s\r\n",ok?"ok":"fail");
 return ok;
}
static uint32_t crc32(const uint8_t *b,size_t n){
 uint32_t c=0xFFFFFFFFu;
 for(size_t i=0;i<n;i++){c^=b[i];for(unsigned k=0;k<8;k++)c=(c&1u)?(c>>1)^0xEDB88320u:(c>>1);}
 return c^0xFFFFFFFFu;
}
static void list_entries(void (*emit)(const char *),const char *tag){
 LfnFatFsScan s;unsigned n=0;bool ok=false;
 if(lfn_fatfs_scan_open(&s,&volume,"")==STORE_IO_OK){
  ok=true;
  for(unsigned i=0;i<SD_WRITE_ENTRIES;i++){
   bool have=false;
   if(lfn_fatfs_scan_next(&s,&entry,&have)!=STORE_IO_OK){ok=false;break;}
   if(!have)break;
   n++;
   say(emit,"LFN SDW %s name=%s bytes=%llu dir=%u\r\n",tag,entry.name,(unsigned long long)entry.bytes,entry.directory?1u:0u);
  }
  (void)lfn_fatfs_scan_close(&s);
 }
 say(emit,"LFN SDW %s count=%u scan=%s\r\n",tag,n,ok?"OK":"FAIL");
}
bool lfn_sd_write_probe_run(void *owner_task, void (*emit)(const char *line)){
 if(sdx.active)return false;
 if(!owner_task||!emit)return false;
 probe_owner=(TaskHandle_t)owner_task;
 bool ok=false;
 if(!lfn_fatfs_volume_init(&volume,"0:",owner_ok,NULL)){
  say(emit,"LFN SDW VOLUME result=fail\r\n");
  say(emit,"LFN SDW DONE result=fail\r\n");
  return false;
 }
 if(lfn_fatfs_mount(&volume)!=STORE_IO_OK){
  say(emit,"LFN SDW MOUNT result=fail\r\n");
  say(emit,"LFN SDW DONE result=fail\r\n");
  return false;
 }
 say(emit,"LFN SDW MOUNT result=ok\r\n");
 list_entries(emit,"BEFORE");
 unsigned token=(unsigned)(xTaskGetTickCount()^0x5A5A5A5A) & 0xFFFFFu;
 char name[16];
 (void)snprintf(name,sizeof(name),"SDT%05X.TMP",token);
 bool exists=false,directory=false;uint64_t bytes=0;
 if(lfn_fatfs_stat(&volume,name,&exists,&directory,&bytes)!=STORE_IO_OK){
  say(emit,"LFN SDW STAT result=fail name=%s\r\n",name);
  goto unmount;
 }
 if(exists){
  say(emit,"LFN SDW NAME_COLLISION name=%s\r\n",name);
  goto unmount;
 }
 say(emit,"LFN SDW NAME name=%s free_check=missing\r\n",name);
 for(size_t i=0;i<SD_WRITE_BYTES;i++)payload[i]=(uint8_t)((i*31u+token)&0xFFu);
 uint32_t want=crc32(payload,SD_WRITE_BYTES);
 if(lfn_fatfs_open(&file,&volume,name,true,true)!=STORE_IO_OK){
  say(emit,"LFN SDW CREATE result=fail\r\n");
  goto unmount;
 }
 say(emit,"LFN SDW CREATE result=ok mode=CREATE_NEW\r\n");
 if(!lfn_fatfs_backend(&file,&backend)){say(emit,"LFN SDW BACKEND result=fail\r\n");goto close_file;}
 size_t actual=0;
 if(backend.write(backend.context,0,payload,SD_WRITE_BYTES,&actual)!=STORE_IO_OK||actual!=SD_WRITE_BYTES){
  say(emit,"LFN SDW WRITE result=fail actual=%u\r\n",(unsigned)actual);
  goto close_file;
 }
 say(emit,"LFN SDW WRITE result=ok bytes=%u\r\n",(unsigned)actual);
 if(lfn_fatfs_close(&file)!=STORE_IO_OK){say(emit,"LFN SDW CLOSE result=fail\r\n");goto unmount;}
 say(emit,"LFN SDW CLOSE result=ok\r\n");
 if(lfn_fatfs_unmount(&volume)!=STORE_IO_OK){say(emit,"LFN SDW UNMOUNT1 result=fail\r\n");goto done_fail;}
 say(emit,"LFN SDW UNMOUNT1 result=ok\r\n");
 if(lfn_fatfs_mount(&volume)!=STORE_IO_OK){say(emit,"LFN SDW REMOUNT result=fail\r\n");goto done_fail;}
 say(emit,"LFN SDW REMOUNT result=ok\r\n");
 {
  bool e2=false,d2=false;uint64_t b2=0;
  if(lfn_fatfs_stat(&volume,name,&e2,&d2,&b2)!=STORE_IO_OK||!e2||d2||b2!=SD_WRITE_BYTES){
   say(emit,"LFN SDW REOPEN_STAT result=fail exists=%d dir=%d bytes=%llu\r\n",e2?1:0,d2?1:0,(unsigned long long)b2);
   goto unmount2;
  }
  say(emit,"LFN SDW REOPEN_STAT result=ok bytes=%llu\r\n",(unsigned long long)b2);
 }
 if(lfn_fatfs_open(&file,&volume,name,false,false)!=STORE_IO_OK){say(emit,"LFN SDW REOPEN result=fail\r\n");goto unmount2;}
 say(emit,"LFN SDW REOPEN result=ok\r\n");
 {
  size_t got=0;
  if(!lfn_fatfs_backend(&file,&backend)||backend.read(backend.context,0,readback,SD_WRITE_BYTES,&got)!=STORE_IO_OK||got!=SD_WRITE_BYTES){
   say(emit,"LFN SDW READBACK result=fail bytes=%u\r\n",(unsigned)got);
   (void)lfn_fatfs_close(&file);goto unmount2;
  }
  uint32_t got_crc=crc32(readback,SD_WRITE_BYTES);
  say(emit,"LFN SDW READBACK result=ok bytes=%u crc_write=%08lX crc_read=%08lX match=%d\r\n",
      (unsigned)got,(unsigned long)want,(unsigned long)got_crc,got_crc==want?1:0);
  (void)lfn_fatfs_close(&file);
  if(got_crc!=want){goto unmount2;}
 }
 list_entries(emit,"AFTER");
 if(lfn_fatfs_unmount(&volume)!=STORE_IO_OK){say(emit,"LFN SDW UNMOUNT2 result=fail\r\n");goto done_fail;}
 say(emit,"LFN SDW UNMOUNT2 result=ok\r\n");
 ok=true;
 goto done;
unmount2:
 (void)lfn_fatfs_unmount(&volume);
 goto done_fail;
close_file:
 (void)lfn_fatfs_close(&file);
unmount:
 (void)lfn_fatfs_unmount(&volume);
done_fail:
 say(emit,"LFN SDW DONE result=fail\r\n");
 return false;
done:
 say(emit,"LFN SDW FILE name=%s bytes=%u crc=%08lX\r\n",name,(unsigned)SD_WRITE_BYTES,(unsigned long)want);
 say(emit,"LFN SDW DONE result=ok\r\n");
 return ok;
}
static const char *const cleanup_names[]={
 "LFREE140.LFR","LFR0FA7C.LFR","LFR2D0FA.LFR","LFR0B7BE.LFR","LFR31212.LFR",
 "LFR81010.LFR","LFR70372.LFR","LFR63992.LFR","LFR67DF6.LFR",
 "LFNM2/AMON.LFR","LFNM2/ARAW.LFR","LFNM2/AIDX.SFL","LFNM2/ADIR.SFC",
 "LFNM2/BMON.LFR","LFNM2/BRAW.LFR","LFNM2/BIDX.SFL","LFNM2/BDIR.SFC",
 "LFNM2/CMON.LFR","LFNM2/CRAW.LFR","LFNM2/CIDX.SFL","LFNM2/CDIR.SFC"};
bool lfn_sd_cleanup_run(void *owner_task, void (*emit)(const char *line)){
 if(sdx.active)return false;
 if(!owner_task||!emit)return false;
 probe_owner=(TaskHandle_t)owner_task;
 if(!lfn_fatfs_volume_init(&volume,"0:",owner_ok,NULL)){
  say(emit,"LFN SDD VOLUME result=fail\r\n");say(emit,"LFN SDD DONE result=fail\r\n");return false;
 }
 if(lfn_fatfs_mount(&volume)!=STORE_IO_OK){
  say(emit,"LFN SDD MOUNT result=fail\r\n");say(emit,"LFN SDD DONE result=fail\r\n");return false;
 }
 say(emit,"LFN SDD MOUNT result=ok\r\n");
 list_entries(emit,"BEFORE");
 unsigned removed=0,missing=0,failed=0;
 for(unsigned k=0;k<sizeof(cleanup_names)/sizeof(cleanup_names[0]);k++){
  const char *name=cleanup_names[k];
  bool exists=false,directory=false;uint64_t bytes=0;
  if(lfn_fatfs_stat(&volume,name,&exists,&directory,&bytes)!=STORE_IO_OK){
   say(emit,"LFN SDD DELETE name=%s result=stat_fail\r\n",name);failed++;continue;
  }
  if(!exists){say(emit,"LFN SDD DELETE name=%s result=missing\r\n",name);missing++;continue;}
  if(directory){say(emit,"LFN SDD DELETE name=%s result=refused_directory\r\n",name);failed++;continue;}
  bool already=false;
  StorageIoResult r=lfn_fatfs_remove(&volume,name,&already);
  say(emit,"LFN SDD DELETE name=%s bytes=%llu result=%s\r\n",name,(unsigned long long)bytes,
      r==STORE_IO_OK?(already?"already_missing":"ok"):"fail");
  if(r==STORE_IO_OK)removed++;else failed++;
 }
 list_entries(emit,"AFTER");
 StorageIoResult u=lfn_fatfs_unmount(&volume);
 say(emit,"LFN SDD UNMOUNT result=%d\r\n",(int)u);
 bool ok=(u==STORE_IO_OK&&failed==0);
 say(emit,"LFN SDD SUMMARY removed=%u missing=%u failed=%u\r\n",removed,missing,failed);
 say(emit,"LFN SDD DONE result=%s\r\n",ok?"ok":"fail");
 return ok;
}
bool lfn_sd_mkdir_probe_run(void *owner_task, void (*emit)(const char *line), const char *dir){
 if(sdx.active)return false;
 if(!owner_task||!emit||!dir||!*dir||strlen(dir)>8)return false;
 probe_owner=(TaskHandle_t)owner_task;
 bool ok=false;
 if(!lfn_fatfs_volume_init(&volume,"0:",owner_ok,NULL)){say(emit,"LFN SDM MKDIR result=volume_fail\r\n");return false;}
 if(lfn_fatfs_mount(&volume)!=STORE_IO_OK){say(emit,"LFN SDM MKDIR result=mount_fail\r\n");return false;}
 char path[LFN_FATFS_PATH];(void)snprintf(path,sizeof(path),"0:/%s",dir);
 FRESULT r=f_mkdir(path);
 bool created=(r==FR_OK),exists=(r==FR_EXIST);
 say(emit,"LFN SDM MKDIR dir=%s created=%d existing=%d fr=%d\r\n",dir,created?1:0,exists?1:0,(int)r);
 ok=created||exists;
 StorageIoResult u=lfn_fatfs_unmount(&volume);
 say(emit,"LFN SDM MKDIR UNMOUNT result=%d\r\n",(int)u);
 return ok&&u==STORE_IO_OK;
}
bool lfn_sd_list_probe_run(void *owner_task, void (*emit)(const char *line), const char *dir){
 if(sdx.active)return false;
 if(!owner_task||!emit||!dir||!*dir)return false;
 probe_owner=(TaskHandle_t)owner_task;
 bool ok=false;
 if(!lfn_fatfs_volume_init(&volume,"0:",owner_ok,NULL)){say(emit,"LFN SDL result=volume_fail\r\n");return false;}
 if(lfn_fatfs_mount(&volume)!=STORE_IO_OK){say(emit,"LFN SDL result=mount_fail\r\n");return false;}
 LfnFatFsScan s;unsigned n=0;bool scan_ok=true;
 if(lfn_fatfs_scan_open(&s,&volume,dir)==STORE_IO_OK){
  for(unsigned i=0;i<SD_PROBE_SCAN_LIMIT;i++){
   bool have=false;
   if(lfn_fatfs_scan_next(&s,&entry,&have)!=STORE_IO_OK){scan_ok=false;break;}
   if(!have)break;
   n++;
   if(n<=SD_PROBE_ENTRIES)say(emit,"LFN SDL ENTRY dir=%s name=%s bytes=%llu isdir=%u\r\n",dir,entry.name,(unsigned long long)entry.bytes,entry.directory?1u:0u);
  }
  (void)lfn_fatfs_scan_close(&s);
 }else scan_ok=false;
 say(emit,"LFN SDL SUMMARY dir=%s entries=%u scan=%s\r\n",dir,n,scan_ok?"OK":"FAIL");
 ok=scan_ok&&lfn_fatfs_unmount(&volume)==STORE_IO_OK;
 say(emit,"LFN SDL DONE result=%s\r\n",ok?"ok":"fail");
 return ok;
}
bool lfn_sd_purge_run(void *owner_task, void (*emit)(const char *line), const char *dir){
 if(sdx.active)return false;
 if(!owner_task||!emit||!dir||!*dir||strlen(dir)>8)return false;
 bool allowed=!strcmp(dir,"LFNM2")||!strncmp(dir,"MW",2)||!strncmp(dir,"MF",2)||!strncmp(dir,"SR",2);
 if(!allowed){say(emit,"LFN SDP REFUSE dir=%s reason=name\r\n",dir);return false;}
 probe_owner=(TaskHandle_t)owner_task;
 if(!lfn_fatfs_volume_init(&volume,"0:",owner_ok,NULL)){say(emit,"LFN SDP result=volume_fail\r\n");return false;}
 if(lfn_fatfs_mount(&volume)!=STORE_IO_OK){say(emit,"LFN SDP result=mount_fail\r\n");return false;}
 LfnFatFsScan s;unsigned files=0,dirs=0,removed=0,failed=0;bool scan_ok=true;
 if(lfn_fatfs_scan_open(&s,&volume,dir)==STORE_IO_OK){
  for(unsigned i=0;i<SD_PROBE_SCAN_LIMIT;i++){
   bool have=false;if(lfn_fatfs_scan_next(&s,&entry,&have)!=STORE_IO_OK){scan_ok=false;break;}
   if(!have)break;
   if(entry.directory){dirs++;continue;}
   files++;
   char path[64];(void)snprintf(path,sizeof(path),"%.31s/%.12s",dir,entry.name);
   bool missing=false;StorageIoResult r=lfn_fatfs_remove(&volume,path,&missing);
   if(r==STORE_IO_OK)removed++;else{failed++;say(emit,"LFN SDP DELETE name=%s result=fail\r\n",entry.name);}
  }
  (void)lfn_fatfs_scan_close(&s);
 }else scan_ok=false;
 bool dir_removed=false;
 if(scan_ok&&!dirs&&!failed){bool missing=false;dir_removed=lfn_fatfs_remove(&volume,dir,&missing)==STORE_IO_OK;}
 say(emit,"LFN SDP SUMMARY dir=%s files=%u removed=%u subdirs=%u failed=%u dir_removed=%d scan=%s\r\n",
     dir,files,removed,dirs,failed,dir_removed?1:0,scan_ok?"OK":"FAIL");
 StorageIoResult u=lfn_fatfs_unmount(&volume);
 bool ok=u==STORE_IO_OK&&scan_ok&&!failed&&!dirs&&dir_removed;
 say(emit,"LFN SDP DONE result=%s\r\n",ok?"ok":"fail");
 return ok;
}
static bool purge_dir_files(void (*emit)(const char *line),const char *dir,unsigned depth){
 char path[96];LfnFatFsScan scan;
 if(lfn_fatfs_scan_open(&scan,&volume,dir)!=STORE_IO_OK)return false;
 bool ok=true;LfnFatFsEntry e;bool have=false;
 for(unsigned i=0;i<SD_PROBE_SCAN_LIMIT;i++){
  if(lfn_fatfs_scan_next(&scan,&e,&have)!=STORE_IO_OK){ok=false;break;}
  if(!have)break;
  if(e.directory){
   if(depth>=2){ok=false;continue;}
   (void)snprintf(path,sizeof(path),"%.31s/%.12s",dir,e.name);
   if(!purge_dir_files(emit,path,depth+1))ok=false;
  }else{
   (void)snprintf(path,sizeof(path),"%.31s/%.12s",dir,e.name);
   bool missing=false;
   if(lfn_fatfs_remove(&volume,path,&missing)!=STORE_IO_OK){ok=false;say(emit,"LFN SDC FAIL delete=%s\r\n",path);}
  }
 }
 (void)lfn_fatfs_scan_close(&scan);
 if(ok){bool missing=false;if(lfn_fatfs_remove(&volume,dir,&missing)!=STORE_IO_OK)ok=false;}
 return ok;
}
bool lfn_sd_purge_all_run(void *owner_task,void (*emit)(const char *line)){
 if(sdx.active)return false;
 if(!owner_task||!emit)return false;
 probe_owner=(TaskHandle_t)owner_task;
 if(!lfn_fatfs_volume_init(&volume,"0:",owner_ok,NULL)){say(emit,"LFN SDC result=volume_fail\r\n");return false;}
 if(lfn_fatfs_mount(&volume)!=STORE_IO_OK){say(emit,"LFN SDC result=mount_fail\r\n");return false;}
 static const char *keep[2]={"SR41F55E","SR73B140"};
 LfnFatFsScan scan;bool ok=true;unsigned dirs=0,removed=0,failed=0;
 if(lfn_fatfs_scan_open(&scan,&volume,"")!=STORE_IO_OK)ok=false;
 if(ok){
  LfnFatFsEntry e;bool have=false;
  for(unsigned i=0;i<SD_PROBE_SCAN_LIMIT;i++){
   if(lfn_fatfs_scan_next(&scan,&e,&have)!=STORE_IO_OK){ok=false;break;}
   if(!have)break;
   if(!e.directory)continue;
   bool match=!strcmp(e.name,"LFNM2")||!strncmp(e.name,"MW",2)||!strncmp(e.name,"MF",2)||!strncmp(e.name,"SR",2);
   bool kept=false;for(unsigned k=0;k<2;k++)if(!strcmp(e.name,keep[k]))kept=true;
   if(!match||kept)continue;
   dirs++;
   if(purge_dir_files(emit,e.name,1))removed++;else failed++;
  }
  (void)lfn_fatfs_scan_close(&scan);
 }
 StorageIoResult u=lfn_fatfs_unmount(&volume);
 bool pass=ok&&!failed&&u==STORE_IO_OK;
 say(emit,"LFN SDC SUMMARY dirs=%u removed=%u failed=%u scan=%s\n",dirs,removed,failed,(ok&&u==STORE_IO_OK)?"OK":"FAIL");
 say(emit,"LFN SDC DONE result=%s\n",pass?"ok":"fail");
 return pass;
}
