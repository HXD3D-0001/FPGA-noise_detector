#include "fatfs_backend.h"
#include <string.h>
#if FF_FS_READONLY != 0 || FF_FS_MINIMIZE != 0
#error E storage requires writable full FatFs API
#endif
static bool owner(LfnFatFsVolume *v){return v&&v->is_owner&&v->is_owner(v->owner_context);}
static bool permit(LfnFatFsVolume *v,const char *full,bool change){
 if(!v->permit)return true;
 const char *relative=full?full+strlen(v->drive)+1:NULL;
 return v->permit(v->guard_context,relative,change);
}
static volatile unsigned last_fr,last_open_stage;
unsigned lfn_fatfs_last_error(void){return last_fr;}
unsigned lfn_fatfs_last_open_stage(void){return last_open_stage;}
static StorageIoResult result(FRESULT r){last_fr=(unsigned)r;return r==FR_OK?STORE_IO_OK:r==FR_NOT_READY||r==FR_INVALID_DRIVE?STORE_IO_NO_MEDIA:STORE_IO_ERROR;}
static bool path(LfnFatFsVolume *v,const char *relative,bool empty,char out[LFN_FATFS_PATH]){
 if(!v||!relative||(!*relative&&!empty))return false;
 size_t used=strlen(v->drive);memcpy(out,v->drive,used);out[used++]='/';size_t start=used,base=0,extension=0;bool dot=false;
 for(size_t k=0;;k++){
  unsigned char c=(unsigned char)relative[k];
  if(!c||c=='/'){
   size_t n=used-start;if(!n){if(!c&&empty&&!k){out[used]=0;return true;}return false;}
   if((n==1&&out[start]=='.')||(n==2&&out[start]=='.'&&out[start+1]=='.'))return false;
#if FF_USE_LFN == 0
   if(!base||base>8||extension>3||(dot&&!extension))return false;
#endif
   if(!c){out[used]=0;return true;}if(used+1>=LFN_FATFS_PATH)return false;
 out[used++]='/';start=used;base=extension=0;dot=false;continue;
  }
  if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.')||used+1>=LFN_FATFS_PATH)return false;
  if(c=='.'){
#if FF_USE_LFN == 0
   if(dot)return false;
#endif
   dot=true;
  }else if(dot)extension++;else base++;
  out[used++]=(char)(c>='a'&&c<='z'?c-'a'+'A':c);
 }
}
bool lfn_fatfs_volume_init(LfnFatFsVolume *v,const char *drive,bool (*check)(void *),void *ctx){
 if(!v||!drive||!check)return false;
 size_t n=0;unsigned logical=0;while(n<6&&drive[n]>='0'&&drive[n]<='9'){logical=logical*10+(unsigned)(drive[n]-'0');n++;}
 if(n!=1||drive[n]!=':'||drive[n+1]||logical>=FF_VOLUMES)return false;
 char name[8]={0};memcpy(name,drive,n+1);memset(v,0,sizeof(*v));memcpy(v->drive,name,8);v->is_owner=check;v->owner_context=ctx;return true;
}
StorageIoResult lfn_fatfs_mount(LfnFatFsVolume *v){if(!owner(v)||v->mounted||!permit(v,NULL,true))return STORE_IO_ERROR;
 StorageIoResult r=result(f_mount(&v->fs,v->drive,1));if(r==STORE_IO_OK)v->mounted=true;return r;}
StorageIoResult lfn_fatfs_unmount(LfnFatFsVolume *v){if(!owner(v)||!v->mounted||v->count||v->scans||!permit(v,NULL,true))return STORE_IO_ERROR;
 StorageIoResult r=result(f_mount(NULL,v->drive,0));if(r==STORE_IO_OK)v->mounted=false;return r;}
StorageIoResult lfn_fatfs_open(LfnFatFsFile *f,LfnFatFsVolume *v,const char *relative,bool create,bool writable){
 last_open_stage=1;
 if(!f)return STORE_IO_ERROR;
 last_open_stage=2;
 if(!owner(v))return STORE_IO_ERROR;
 last_open_stage=3;
 if(!v->mounted)return STORE_IO_ERROR;
 last_open_stage=4;
 if(v->count==LFN_FATFS_HANDLES)return STORE_IO_ERROR;
 last_open_stage=5;
 if(create&&!writable)return STORE_IO_ERROR;
 char full[LFN_FATFS_PATH];
 last_open_stage=6;
 if(!path(v,relative,false,full))return STORE_IO_ERROR;
 last_open_stage=7;
 if(writable&&(!permit(v,full,true)||!permit(v,full,false)))return STORE_IO_ERROR;
 last_open_stage=8;
 for(unsigned k=0;k<LFN_FATFS_HANDLES;k++)if(v->handles[k]&&(v->handles[k]==f||!strcmp(v->handles[k]->path,full)))return STORE_IO_ERROR;
 last_open_stage=9;
 memset(f,0,sizeof(*f));f->volume=v;f->writable=writable;memcpy(f->path,full,strlen(full)+1);
 StorageIoResult r=result(f_open(&f->file,full,(BYTE)(FA_READ|(writable?FA_WRITE:0)|(create?FA_CREATE_NEW:FA_OPEN_EXISTING))));if(r!=STORE_IO_OK)return r;
 f->opened=true;for(unsigned k=0;k<LFN_FATFS_HANDLES;k++)if(!v->handles[k]){v->handles[k]=f;v->count++;break;}return STORE_IO_OK;
}
StorageIoResult lfn_fatfs_close(LfnFatFsFile *f){
 if(!f||!f->opened||!owner(f->volume))return STORE_IO_ERROR;
 if(f->writable){if(!permit(f->volume,f->path,false))return STORE_IO_ERROR;StorageIoResult r=result(f_sync(&f->file));if(r!=STORE_IO_OK)return r;}
 StorageIoResult r=result(f_close(&f->file));if(r!=STORE_IO_OK)return r;
 LfnFatFsVolume *v=f->volume;for(unsigned k=0;k<LFN_FATFS_HANDLES;k++)if(v->handles[k]==f){v->handles[k]=NULL;v->count--;break;}f->opened=false;return STORE_IO_OK;
}
static bool file_owner(LfnFatFsFile *f){return f&&f->opened&&owner(f->volume)&&f->volume->mounted;}
static StorageIoResult size_io(void *ctx,uint64_t *n){LfnFatFsFile *f=ctx;if(!n||!file_owner(f))return STORE_IO_ERROR;
 *n=f_size(&f->file);return STORE_IO_OK;}
static StorageIoResult read_io(void *ctx,uint64_t offset,uint8_t *b,size_t n,size_t *actual){
 LfnFatFsFile *f=ctx;if(!actual||!b||!file_owner(f)||n>STORAGE_IO_CHUNK||offset>(uint64_t)f_size(&f->file)||n>(uint64_t)f_size(&f->file)-offset)return STORE_IO_ERROR;
 *actual=0;StorageIoResult r=result(f_lseek(&f->file,(FSIZE_t)offset));if(r!=STORE_IO_OK)return r;UINT got=0;r=result(f_read(&f->file,b,(UINT)n,&got));*actual=got;return r;
}
static StorageIoResult write_io(void *ctx,uint64_t offset,const uint8_t *b,size_t n,size_t *actual){
 LfnFatFsFile *f=ctx;if(!actual||!b||!file_owner(f)||!f->writable||n>STORAGE_IO_CHUNK||offset>(uint64_t)f_size(&f->file)||offset>(uint64_t)(FSIZE_t)-1||n>(uint64_t)(FSIZE_t)-1-offset)return STORE_IO_ERROR;
 *actual=0;if(!permit(f->volume,f->path,false)||(offset<(uint64_t)f_size(&f->file)&&!permit(f->volume,f->path,true)))return STORE_IO_ERROR;StorageIoResult r=result(f_lseek(&f->file,(FSIZE_t)offset));if(r!=STORE_IO_OK)return r;UINT wrote=0;r=result(f_write(&f->file,b,(UINT)n,&wrote));*actual=wrote;return r;
}
static StorageIoResult sync_io(void *ctx){LfnFatFsFile *f=ctx;return file_owner(f)&&f->writable&&permit(f->volume,f->path,false)?result(f_sync(&f->file)):STORE_IO_ERROR;}
static StorageIoResult truncate_io(void *ctx,uint64_t n){LfnFatFsFile *f=ctx;if(!file_owner(f)||!f->writable||n>(uint64_t)f_size(&f->file)||!permit(f->volume,f->path,true)||!permit(f->volume,f->path,false))return STORE_IO_ERROR;
 StorageIoResult r=result(f_lseek(&f->file,(FSIZE_t)n));return r==STORE_IO_OK?result(f_truncate(&f->file)):r;}
bool lfn_fatfs_backend(LfnFatFsFile *f,StorageBackend *b){if(!b||!file_owner(f))return false;
 *b=(StorageBackend){f,size_io,read_io,write_io,sync_io,truncate_io,NULL};return true;}

StorageIoResult lfn_fatfs_mkdir(LfnFatFsVolume *v,const char *relative,bool existing){
 char full[LFN_FATFS_PATH];if(!owner(v)||!v->mounted||!path(v,relative,false,full))return STORE_IO_ERROR;
 if(!permit(v,full,true))return STORE_IO_ERROR;
 FRESULT r=f_mkdir(full);if(r==FR_EXIST&&existing){FILINFO info;r=f_stat(full,&info);if(r==FR_OK&&!(info.fattrib&AM_DIR))return STORE_IO_ERROR;}return result(r);
}
StorageIoResult lfn_fatfs_free_bytes(LfnFatFsVolume *v,uint64_t *out){
 if(!out||!owner(v)||!v->mounted)return STORE_IO_ERROR;
 DWORD clusters=0;FATFS *fs=NULL;StorageIoResult r=result(f_getfree(v->drive,&clusters,&fs));if(r!=STORE_IO_OK)return r;if(fs!=&v->fs)return STORE_IO_ERROR;
#if FF_MIN_SS == FF_MAX_SS
 uint64_t sector=FF_MAX_SS;
#else
 uint64_t sector=fs->ssize;
#endif
 if(!fs->csize||!sector||sector>UINT64_MAX/fs->csize||(uint64_t)clusters>UINT64_MAX/(sector*fs->csize))return STORE_IO_ERROR;
 *out=(uint64_t)clusters*sector*fs->csize;return STORE_IO_OK;
}
bool lfn_fatfs_type(LfnFatFsVolume *v,unsigned *out){
 if(!out||!owner(v)||!v->mounted)return false;
 *out=(unsigned)v->fs.fs_type;return true;
}
bool lfn_fatfs_space(LfnFatFsVolume *v,uint64_t *free_bytes,uint64_t *total_bytes){
 if(!free_bytes||!total_bytes||!owner(v)||!v->mounted)return false;
 DWORD clusters=0;FATFS *fs=NULL;FRESULT r=f_getfree(v->drive,&clusters,&fs);
 if(r!=FR_OK||fs!=&v->fs)return false;
#if FF_MIN_SS == FF_MAX_SS
 uint64_t sector=FF_MAX_SS;
#else
 uint64_t sector=fs->ssize;
#endif
 if(!fs->csize||!sector||sector>UINT64_MAX/fs->csize)return false;
 uint64_t unit=(uint64_t)fs->csize*sector;
 if((uint64_t)clusters>UINT64_MAX/unit||!fs->n_fatent||fs->n_fatent<2||
    (uint64_t)(fs->n_fatent-2)>UINT64_MAX/unit)return false;
 *free_bytes=(uint64_t)clusters*unit;
 *total_bytes=(uint64_t)(fs->n_fatent-2)*unit;
 return true;
}
StorageIoResult lfn_fatfs_remove(LfnFatFsVolume *v,const char *relative,bool *missing){
 if(!missing||!owner(v)||!v->mounted)return STORE_IO_ERROR;
 char full[LFN_FATFS_PATH];if(!path(v,relative,false,full))return STORE_IO_ERROR;
 for(unsigned k=0;k<LFN_FATFS_HANDLES;k++)if(v->handles[k]&&!strcmp(v->handles[k]->path,full))return STORE_IO_ERROR;
 if(!permit(v,full,true))return STORE_IO_ERROR;
 FRESULT r=f_unlink(full);if(r==FR_NO_FILE||r==FR_NO_PATH){*missing=true;return STORE_IO_OK;}if(r==FR_OK)*missing=false;return result(r);
}
StorageIoResult lfn_fatfs_scan_open(LfnFatFsScan *s,LfnFatFsVolume *v,const char *relative){
 if(!s||!owner(v)||!v->mounted)return STORE_IO_ERROR;
 char full[LFN_FATFS_PATH];if(!path(v,relative,true,full))return STORE_IO_ERROR;
 memset(s,0,sizeof(*s));s->volume=v;StorageIoResult r=result(f_opendir(&s->directory,full));if(r==STORE_IO_OK){s->opened=true;v->scans++;}return r;
}
StorageIoResult lfn_fatfs_scan_next(LfnFatFsScan *s,LfnFatFsEntry *out,bool *have){
 if(!s||!out||!have||!s->opened||!owner(s->volume))return STORE_IO_ERROR;
 if(s->done){*have=false;return STORE_IO_OK;}
 FILINFO info;StorageIoResult r=result(f_readdir(&s->directory,&info));if(r!=STORE_IO_OK)return r;if(!info.fname[0]){s->done=true;*have=false;return STORE_IO_OK;}
 LfnFatFsEntry entry={0};size_t n=strlen(info.fname);if(n>=sizeof(entry.name))return STORE_IO_ERROR;
 memcpy(entry.name,info.fname,n+1);entry.bytes=info.fsize;entry.directory=(info.fattrib&AM_DIR)!=0;*out=entry;*have=true;return STORE_IO_OK;
}
StorageIoResult lfn_fatfs_scan_close(LfnFatFsScan *s){if(!s||!s->opened||!owner(s->volume))return STORE_IO_ERROR;
 StorageIoResult r=result(f_closedir(&s->directory));if(r==STORE_IO_OK){s->opened=false;s->volume->scans--;}return r;}

StorageIoResult lfn_fatfs_stat(LfnFatFsVolume *v,const char *relative,bool *exists,bool *directory,uint64_t *bytes){
 char full[LFN_FATFS_PATH];if(!exists||!directory||!bytes||!owner(v)||!v->mounted||!path(v,relative,false,full))return STORE_IO_ERROR;
 FILINFO info;FRESULT r=f_stat(full,&info);if(r==FR_NO_FILE||r==FR_NO_PATH){*exists=false;*directory=false;*bytes=0;return STORE_IO_OK;}
 if(r!=FR_OK)return result(r);
 *exists=true;*directory=(info.fattrib&AM_DIR)!=0;*bytes=info.fsize;return STORE_IO_OK;
}