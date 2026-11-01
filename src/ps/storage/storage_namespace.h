#ifndef LFN_STORAGE_NAMESPACE_H
#define LFN_STORAGE_NAMESPACE_H
#include "storage_writer.h"
typedef struct {
 void *context;bool (*is_owner)(void *);char root[192];
 uint64_t version;bool reading,writing;
} StorageNamespace;
/* Single storage executor, no thread locks/IO. One namespace read lease.
 * All creation/mkdir/unlink/unmount and metadata mutations MUST use matching
 * write_begin/end, including paths outside this controller. No direct bypass.
 * Version advances BEFORE write, even a failed mutation invalidates old lookup.
 * Read release is only for the scanner holding this exact guard instance. */
bool storage_namespace_init(StorageNamespace *,const char *root,uint64_t version,bool (*owner)(void *),void *);
StorageResult storage_namespace_read_acquire(void *,const char *,uint64_t *);
uint64_t storage_namespace_version(void *);
void storage_namespace_read_release(void *);
StorageResult storage_namespace_write_begin(StorageNamespace *,uint64_t *token);
StorageResult storage_namespace_write_end(StorageNamespace *,uint64_t token);
#endif
