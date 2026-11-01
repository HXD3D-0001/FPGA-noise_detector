#ifndef LFN_STORAGE_BINDING_H
#define LFN_STORAGE_BINDING_H
#include "storage_metadata.h"
/* Registry is the immutable schema context; raw IO (e.g. FatFs FIL) keeps its
 * own context. Exact storage_registry_validate gate required by service is
 * preserved, unlike a lookalike validator callback. Persistent sole owner. */
typedef struct {StorageMetadataRegistry registry;StorageBackend raw,backend;} StorageBinding;
bool storage_binding_init(StorageBinding *,const StorageBackend *);
#endif
