#include "storage_namespace.h"
#include <string.h>
static bool owned(StorageNamespace *n){return n&&n->is_owner&&n->is_owner(n->context);}
bool storage_namespace_init(StorageNamespace *n,const char *root,uint64_t version,bool (*owner)(void *),void *ctx){
 if(!n||!root||!version||!owner||!owner(ctx))return false;
 char copy[192]={0};size_t len=0;unsigned base=0,ext=0;bool dot=false;
 for(;;){
  if(len>=sizeof(copy)-1)return false;
  unsigned char c=(unsigned char)root[len];
  if(!c||c=='/'){
   if(!base||base>8||ext>3||(dot&&!ext))return false;
   if(!c)break;
   base=ext=0;dot=false;
  }else if(c=='.'){if(dot||!base)return false;dot=true;}
  else{if(!((c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'))return false;if(dot)ext++;else base++;}
  copy[len++]=(char)c;
 }
 memset(n,0,sizeof(*n));n->context=ctx;n->is_owner=owner;n->version=version;memcpy(n->root,copy,192);return true;
}
StorageResult storage_namespace_read_acquire(void *ctx,const char *root,uint64_t *version){
 StorageNamespace *n=ctx;if(!owned(n)||!root||!version||strcmp(root,n->root))return STORE_INVALID;
 if(n->reading||n->writing)return STORE_BUSY;
 n->reading=true;*version=n->version;return STORE_OK;
}
uint64_t storage_namespace_version(void *ctx){StorageNamespace *n=ctx;return owned(n)?n->version:0;}
void storage_namespace_read_release(void *ctx){StorageNamespace *n=ctx;if(owned(n)&&n->reading)n->reading=false;}
StorageResult storage_namespace_write_begin(StorageNamespace *n,uint64_t *token){
 if(!owned(n)||!token)return STORE_INVALID;
 if(n->reading||n->writing)return STORE_BUSY;
 if(n->version==UINT64_MAX)return STORE_EXHAUSTED;
 n->writing=true;*token=++n->version;return STORE_OK;
}
StorageResult storage_namespace_write_end(StorageNamespace *n,uint64_t token){
 if(!owned(n)||!token||!n->writing||token!=n->version)return STORE_INVALID;
 n->writing=false;return STORE_OK;
}
