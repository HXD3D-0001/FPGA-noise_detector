#ifndef LFN_FATFS_FILE_PORT_H
#define LFN_FATFS_FILE_PORT_H
#include "fatfs_backend.h"
#include "storage_file_handles.h"
typedef struct {
 LfnFatFsVolume *volume;LfnFatFsFile files[STORAGE_HANDLES];
 void *sync_context;StorageIoResult (*media_sync)(void *);
} LfnFatFsFilePort;
/* Existing explicitly mounted volume. No mount, formatting, controller choice.
 * media_sync must flush the matching actual medium; never substitute noop.
 * Files are owned by the port; do not also open paths through another owner.
 * All StorageFileHandles objects must share these exact ops/slot indices. */
bool lfn_fatfs_file_port_init(LfnFatFsFilePort *,LfnFatFsVolume *,
 StorageIoResult (*media_sync)(void *),void *,StorageFileOps *);
/* Factory-only initial create. Register spec first, then create; on success
 * raw backend is attached active. Failure keeps any opened file in port for
 * inspection/explicit close, never removes an existing path or overwrites it.
 * raw adapter still requires production registry binding before writer use. */
StorageResult lfn_fatfs_file_port_create(LfnFatFsFilePort *,StorageFileHandles *,unsigned);
#endif
