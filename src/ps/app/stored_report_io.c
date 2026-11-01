#include "stored_report_io.h"
#include "../storage/fatfs_backend.h"
static LfnFatFsVolume volume;
static LfnFatFsFile file;
static uint8_t frame[RECORD_MAX_BYTES] __attribute__((section(".lfn_data.report"),aligned(64)));
static bool read_exact(StorageBackend *io,uint64_t offset,uint8_t *dst,size_t length){
 while(length){
  size_t count=length>STORAGE_IO_CHUNK?STORAGE_IO_CHUNK:length,got=0;
  if(io->read(io->context,offset,dst,count,&got)!=STORE_IO_OK||got!=count)return false;
  offset+=count;dst+=count;length-=count;
 }
 return true;
}
bool lfn_stored_report_read(LfnSavedReport *report,const char *path,bool (*owner)(void *),void *context){
 if(!report||!path||!owner||!owner(context))return false;
 if(volume.mounted)return false;
 lfn_saved_report_init(report);
 if(!lfn_fatfs_volume_init(&volume,"0:",owner,context)||lfn_fatfs_mount(&volume)!=STORE_IO_OK)return false;
 bool opened=lfn_fatfs_open(&file,&volume,path,false,false)==STORE_IO_OK,ok=opened;
 StorageBackend io;uint64_t size=0,offset=0;
 if(ok)ok=lfn_fatfs_backend(&file,&io)&&io.size(io.context,&size)==STORE_IO_OK;
 while(ok&&offset<size){
  if(size-offset<80||!read_exact(&io,offset,frame,64)){ok=false;break;}
  uint32_t payload=(uint32_t)frame[12]|((uint32_t)frame[13]<<8)|((uint32_t)frame[14]<<16)|((uint32_t)frame[15]<<24);
  if(payload>RECORD_MAX_BYTES-80||payload+80>size-offset){ok=false;break;}
  size_t length=(size_t)payload+80;RecordView view;
  ok=read_exact(&io,offset+64,frame+64,length-64)&&record_decode(frame,length,&view)&&lfn_saved_report_add(report,&view);
  offset+=length;
 }
 if(opened&&lfn_fatfs_close(&file)!=STORE_IO_OK)ok=false;
 if(lfn_fatfs_unmount(&volume)!=STORE_IO_OK)ok=false;
 return ok&&size!=0;
}
