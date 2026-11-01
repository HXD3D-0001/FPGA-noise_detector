#ifndef LFN_FATFS_NAMESPACE_PORT_H
#define LFN_FATFS_NAMESPACE_PORT_H
#include "fatfs_backend.h"
#include "storage_namespace.h"
enum { LFN_NAMESPACE_NAMES=16 };
typedef struct {char name[13];bool snapshot_metadata;} LfnFatFsNamespaceName;
typedef struct {LfnFatFsVolume *volume;StorageNamespace *space;unsigned count;LfnFatFsNamespaceName names[LFN_NAMESPACE_NAMES];} LfnFatFsNamespacePort;
/* Bind once to an initialized, UNMOUNTED volume with zero handles/scans.
 * Explicit basename allowlist covers all epochs under this namespace root.
 * Catalog/delete/boot metadata MUST be marked snapshot_metadata. Data/SFL may
 * append during snapshot; sealed-file immutability remains service's job.
 * No IO, no mount, no token acquisition or automatic release on failure.
 * Object,volume,space live together; do not reinit/unbind while in use.
 * Caller obtains write_begin before volume/path mutations and metadata IO;
 * lifecycle holds it across factory/bootstrap/rotation. Raw f_* bypass forbidden. */
bool lfn_fatfs_namespace_port_bind(LfnFatFsNamespacePort *,LfnFatFsVolume *,StorageNamespace *,const LfnFatFsNamespaceName *,unsigned);
/* Pure identity check; rejects an unbound/replaced volume guard. */
bool lfn_fatfs_namespace_port_matches(LfnFatFsNamespacePort *,LfnFatFsVolume *,StorageNamespace *);
#endif
