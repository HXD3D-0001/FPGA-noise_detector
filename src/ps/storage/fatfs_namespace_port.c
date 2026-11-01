#include "fatfs_namespace_port.h"
#include <string.h>
static bool permit(void *ctx,const char *path,bool change){
 LfnFatFsNamespacePort *p=ctx;StorageNamespace *s=p->space;
 if(!s->is_owner||!s->is_owner(s->context)||!s->version)return false;
 bool write=s->writing&&!s->reading;
 if(!path)return change&&write;
 /* FatFs backend passes drive-qualified "0:/..." paths; the namespace root is
  * relative. Strip only the exact drive prefix, never arbitrary components. */
 if(path[0]>='0'&&path[0]<='9'&&path[1]==':'&&path[2]=='/')path+=3;
 size_t n=strlen(s->root);if(strncmp(path,s->root,n)||(path[n]&&path[n]!='/'))return false;
 if(change)return write;
 const char *base=strrchr(path,'/');base=base?base+1:path;
 for(unsigned k=0;k<p->count;k++)if(!strcmp(base,p->names[k].name))return !p->names[k].snapshot_metadata||write;
 return false;
}
static bool valid(const char *s){
 unsigned base=0,ext=0;bool dot=false;
 for(unsigned k=0;k<13;k++){unsigned char c=(unsigned char)s[k];if(!c)return base&&base<=8&&ext<=3&&(!dot||ext);
 if(c=='.'){if(dot||!base)return false;dot=true;}else{if(!((c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'))return false;if(dot)ext++;else base++;}}
 return false;
}
bool lfn_fatfs_namespace_port_bind(LfnFatFsNamespacePort *p,LfnFatFsVolume *v,StorageNamespace *s,const LfnFatFsNamespaceName *names,unsigned count){
 if(!p||!v||!s||!names||!count||count>LFN_NAMESPACE_NAMES||v->mounted||v->count||v->scans||v->permit||!v->is_owner||!v->is_owner(v->owner_context)||!storage_namespace_version(s)||s->reading||s->writing)return false;
 for(unsigned k=0;k<count;k++){if(!valid(names[k].name))return false;for(unsigned j=0;j<k;j++)if(!strcmp(names[k].name,names[j].name))return false;}
 LfnFatFsNamespaceName copy[LFN_NAMESPACE_NAMES]={0};memcpy(copy,names,count*sizeof(*names));memset(p,0,sizeof(*p));p->volume=v;p->space=s;p->count=count;memcpy(p->names,copy,sizeof(copy));v->guard_context=p;v->permit=permit;return true;
}

bool lfn_fatfs_namespace_port_matches(LfnFatFsNamespacePort *p,LfnFatFsVolume *v,StorageNamespace *s){return p&&v&&s&&p->volume==v&&p->space==s&&v->guard_context==p&&v->permit==permit&&v->is_owner&&v->is_owner(v->owner_context)&&storage_namespace_version(s);}