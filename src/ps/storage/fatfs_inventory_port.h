#ifndef LFN_FATFS_INVENTORY_PORT_H
#define LFN_FATFS_INVENTORY_PORT_H
#include "fatfs_backend.h"
#include "storage_inventory.h"
typedef struct {
 void *context;StorageResult (*acquire)(void *,const char *,uint64_t *);
 uint64_t (*version)(void *);void (*release)(void *);
} StorageNamespaceGuard;
typedef struct {
 LfnFatFsVolume *volume;StorageNamespaceGuard guard;
 LfnFatFsScan directories[STORAGE_INVENTORY_DEPTH];LfnFatFsFile metadata;
} LfnFatFsInventoryPort;
/* Explicit already-mounted volume and real namespace guard. Single persistent
 * reader file,8 directory handles. No card/controller/volume/filename defaults,
 * no mkdir, format, repair or writes. Owner must prevent path/metadata mutation
 * through every OTHER file port while guard held, and bump version on changes. */
bool lfn_fatfs_inventory_port_init(LfnFatFsInventoryPort *,LfnFatFsVolume *,
 const StorageNamespaceGuard *,StorageInventoryOps *);
#endif
