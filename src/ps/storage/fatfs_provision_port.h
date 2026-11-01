#ifndef LFN_FATFS_PROVISION_PORT_H
#define LFN_FATFS_PROVISION_PORT_H
#include "fatfs_namespace_port.h"
#include "fatfs_file_port.h"
#include "storage_provision.h"
typedef struct {LfnFatFsFilePort *files;LfnFatFsNamespacePort *guard;} LfnFatFsProvisionPort;
/* Already mounted + same bound namespace. No IO/reinitialization of files.
 * Data and ledger names must be ordinary, catalog must snapshot_metadata.
 * This adapter supplies CREATE_NEW and the existing actual media_sync hook.
 * Layout/root/directory/IDs and SD facts are provided by caller. */
bool lfn_fatfs_provision_port_init(LfnFatFsProvisionPort *,LfnFatFsFilePort *,LfnFatFsNamespacePort *,StorageProvisionOps *);
#endif
