#include "storage_binding.h"
#include <stddef.h>
static StorageBinding *binding(void *v){return (StorageBinding*)((uint8_t*)v - offsetof(StorageBinding,registry));}
static StorageIoResult size_io(void *v,uint64_t *n){StorageBinding *b=binding(v);return b->raw.size(b->raw.context,n);}
static StorageIoResult read_io(void *v,uint64_t o,uint8_t *p,size_t n,size_t *a){StorageBinding *b=binding(v);return b->raw.read(b->raw.context,o,p,n,a);}
static StorageIoResult write_io(void *v,uint64_t o,const uint8_t *p,size_t n,size_t *a){StorageBinding *b=binding(v);return b->raw.write(b->raw.context,o,p,n,a);}
static StorageIoResult sync_io(void *v){StorageBinding *b=binding(v);return b->raw.sync(b->raw.context);}
static StorageIoResult truncate_io(void *v,uint64_t n){StorageBinding *b=binding(v);return b->raw.truncate?b->raw.truncate(b->raw.context,n):STORE_IO_ERROR;}
bool storage_binding_init(StorageBinding *b,const StorageBackend *raw){if(!b||!raw||raw==&b->backend||!raw->size||!raw->read||!raw->write||!raw->sync)return false;StorageBackend copy=*raw;b->raw=copy;storage_registry_init(&b->registry);b->backend=(StorageBackend){&b->registry,size_io,read_io,write_io,sync_io,truncate_io,storage_registry_validate};return true;}
