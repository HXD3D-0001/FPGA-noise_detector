#ifndef LFN_FATFS_BACKEND_H
#define LFN_FATFS_BACKEND_H
#include "storage_writer.h"
#include "ff.h"
enum { LFN_FATFS_HANDLES=16,LFN_FATFS_PATH=192 };
typedef struct LfnFatFsFile LfnFatFsFile;
typedef struct {
 FATFS fs;char drive[8];void *owner_context;bool (*is_owner)(void *);
 /* Immutable once bound before mount; NULL preserves legacy isolated ports.
  * permit receives canonical relative path, or NULL for volume operation. */
 void *guard_context;bool (*permit)(void *,const char *,bool namespace_change);
 bool mounted;unsigned count,scans;LfnFatFsFile *handles[LFN_FATFS_HANDLES];
} LfnFatFsVolume;
struct LfnFatFsFile {FIL file;LfnFatFsVolume *volume;char path[LFN_FATFS_PATH];bool opened,writable;};
typedef struct {DIR directory;LfnFatFsVolume *volume;bool opened,done;} LfnFatFsScan;
typedef struct {char name[FF_MAX_LFN+1];uint64_t bytes;bool directory;} LfnFatFsEntry;
/* No implicit volume/card/controller choice, format or mount at init. Every
 * filesystem operation requires the explicit ONE storage-task owner callback.
 * Build against the INSTALLED xilffs headers and matching target BSP. Unknown
 * SD configuration prevents target integration, not Host callback testing. */
bool lfn_fatfs_volume_init(LfnFatFsVolume *,const char *drive,bool (*is_owner)(void *),void *);
StorageIoResult lfn_fatfs_mount(LfnFatFsVolume *);
StorageIoResult lfn_fatfs_unmount(LfnFatFsVolume *);
StorageIoResult lfn_fatfs_open(LfnFatFsFile *,LfnFatFsVolume *,const char *relative_path,bool create_new,bool writable);
StorageIoResult lfn_fatfs_close(LfnFatFsFile *);
/* Raw IO adapter has validate=NULL. Pass to existing storage-task IO wrapper,
 * or storage_binding, which supplies exact production registry schema gate. */
bool lfn_fatfs_backend(LfnFatFsFile *,StorageBackend *);
StorageIoResult lfn_fatfs_mkdir(LfnFatFsVolume *,const char *relative_directory,bool allow_existing);
/* Read-only existence/type/size; missing is success with exists=false.
 * Other errors remain errors; no fallback from media failure to missing. */
StorageIoResult lfn_fatfs_stat(LfnFatFsVolume *,const char *relative_path,bool *exists,bool *directory,uint64_t *bytes);
StorageIoResult lfn_fatfs_free_bytes(LfnFatFsVolume *,uint64_t *);
/* Read-only filesystem type from the mounted FatFs (1/2/3=FS_FAT12/16/32,
 * 4=exFAT, 0=unknown) and total/free space for diagnostics only. */
unsigned lfn_fatfs_last_error(void);
unsigned lfn_fatfs_last_open_stage(void);
bool lfn_fatfs_type(LfnFatFsVolume *,unsigned *);
bool lfn_fatfs_space(LfnFatFsVolume *,uint64_t *free_bytes,uint64_t *total_bytes);
/* Internal storage owner only; retention must first pin exclusion, durable
 * deletion intent and verified closed-file eligibility. No open-path unlink. */
StorageIoResult lfn_fatfs_remove(LfnFatFsVolume *,const char *relative_path,bool *already_missing);
StorageIoResult lfn_fatfs_scan_open(LfnFatFsScan *,LfnFatFsVolume *,const char *relative_directory);
StorageIoResult lfn_fatfs_scan_next(LfnFatFsScan *,LfnFatFsEntry *,bool *have_entry);
StorageIoResult lfn_fatfs_scan_close(LfnFatFsScan *);
#endif