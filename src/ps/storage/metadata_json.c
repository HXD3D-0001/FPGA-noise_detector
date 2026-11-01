#include "metadata_json.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <errno.h>
static void space(MetadataJson*d){while(d->position<d->length&&(d->bytes[d->position]==' '||d->bytes[d->position]=='\t'||d->bytes[d->position]=='\r'||d->bytes[d->position]=='\n'))d->position++;}
static int hex(uint8_t c){return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;}
static bool code(MetadataJson*d,unsigned*out){
 if(d->length-d->position<4)return false;
 unsigned n=0;for(unsigned i=0;i<4;i++){int v=hex(d->bytes[d->position++]);if(v<0)return false;n=n*16+(unsigned)v;}*out=n;return true;
}
static bool string(MetadataJson*d,bool key,int index){
 MetadataJsonToken*t=&d->token[index];d->position++;t->start=(uint16_t)d->position;
 while(d->position<d->length){uint8_t c=d->bytes[d->position++];
  if(c=='"'){t->end=(uint16_t)(d->position-1);return !key||t->end-t->start<=63;}
  if(c<32)return false;
  if(c=='\\'){
   if(key||d->position==d->length)return false;
   c=d->bytes[d->position++];if(c=='u'){unsigned u;if(!code(d,&u))return false;
    if(u>=0xd800&&u<=0xdbff){if(d->length-d->position<2||d->bytes[d->position++]!='\\'||d->bytes[d->position++]!='u')return false;unsigned low;if(!code(d,&low)||low<0xdc00||low>0xdfff)return false;}
    else if(u>=0xdc00&&u<=0xdfff)return false;
   }else if(!strchr("\"\\/bfnrt",c)||!c)return false;
  }else if(c>=128){
   unsigned count;uint32_t cp,min;
   if(c>=0xc2&&c<=0xdf){count=1;cp=c&31;min=0x80;}else if(c>=0xe0&&c<=0xef){count=2;cp=c&15;min=0x800;}else if(c>=0xf0&&c<=0xf4){count=3;cp=c&7;min=0x10000;}else return false;
   if(d->length-d->position<count)return false;
   for(unsigned i=0;i<count;i++){uint8_t b=d->bytes[d->position++];if((b&0xc0)!=0x80)return false;cp=(cp<<6)|(b&63);}
   if(cp<min||cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff))return false;
  }
 }return false;
}
static int value(MetadataJson*d,unsigned depth,bool key){
 space(d);if(depth>12||d->position>=d->length||d->count==STORAGE_METADATA_TOKENS)return -1;
 int index=(int)d->count++;MetadataJsonToken*t=&d->token[index];memset(t,0,sizeof(*t));t->start=(uint16_t)d->position;
 uint8_t c=d->bytes[d->position];
 if(c=='"'){t->type=MJ_STRING;if(!string(d,key,index))return -1;}
 else if(c=='{'||c=='['){
  t->type=c=='{'?MJ_OBJECT:MJ_ARRAY;d->position++;space(d);uint8_t close=c=='{'?'}':']';
  if(d->position<d->length&&d->bytes[d->position]==close)d->position++;
  else for(;;){
   if(c=='{'){
    int k=value(d,depth+1,true);if(k<0||d->token[k].type!=MJ_STRING||t->count>=64)return -1;
    unsigned cursor=(unsigned)index+1;
    for(unsigned n=0;n<t->count;n++){
     MetadataJsonToken*a=&d->token[cursor],*b=&d->token[k];
     if(a->end-a->start==b->end-b->start&&!memcmp(d->bytes+a->start,d->bytes+b->start,a->end-a->start))return -1;
     cursor=d->token[cursor+1].next;
    }
    space(d);if(d->position==d->length||d->bytes[d->position++]!=':')return -1;
   }
   if(value(d,depth+1,false)<0)return -1;
   t->count++;space(d);if(d->position==d->length)return -1;
   uint8_t sep=d->bytes[d->position++];if(sep==close)break;if(sep!=',')return -1;
  }
  t->end=(uint16_t)d->position;
 }else if(c=='-'||(c>='0'&&c<='9')){
  t->type=MJ_NUMBER;if(c=='-'){d->position++;if(d->position==d->length)return -1;}
  if(d->bytes[d->position]=='0')d->position++;
  else{if(d->bytes[d->position]<'1'||d->bytes[d->position]>'9')return -1;while(d->position<d->length&&d->bytes[d->position]>='0'&&d->bytes[d->position]<='9')d->position++;}
  if(d->position<d->length&&d->bytes[d->position]=='.'){d->position++;size_t first=d->position;while(d->position<d->length&&d->bytes[d->position]>='0'&&d->bytes[d->position]<='9')d->position++;if(first==d->position)return -1;}
  if(d->position<d->length&&(d->bytes[d->position]=='e'||d->bytes[d->position]=='E')){d->position++;if(d->position<d->length&&(d->bytes[d->position]=='+'||d->bytes[d->position]=='-'))d->position++;size_t first=d->position;while(d->position<d->length&&d->bytes[d->position]>='0'&&d->bytes[d->position]<='9')d->position++;if(first==d->position)return -1;}
  t->end=(uint16_t)d->position;
 }else{
  t->type=MJ_LITERAL;const char*s=c=='t'?"true":c=='f'?"false":c=='n'?"null":NULL;if(!s)return -1;size_t n=strlen(s);if(d->length-d->position<n||memcmp(d->bytes+d->position,s,n))return -1;d->position+=n;t->end=(uint16_t)d->position;
 }
 t->next=(uint16_t)d->count;return index;
}
bool metadata_json_parse(MetadataJson*d,const uint8_t*b,size_t n){
 if(!d||!b||!n||n>STORAGE_METADATA_BYTES)return false;
 memset(d,0,sizeof(*d));d->bytes=b;d->length=n;int root=value(d,0,false);space(d);return root==0&&d->token[0].type==MJ_OBJECT&&d->position==n;
}
int metadata_json_field(const MetadataJson*d,int parent,const char*name){
 if(parent<0||(unsigned)parent>=d->count||d->token[parent].type!=MJ_OBJECT)return -1;
 unsigned k=(unsigned)parent+1;size_t n=strlen(name);
 for(unsigned i=0;i<d->token[parent].count;i++){const MetadataJsonToken*t=&d->token[k];if((size_t)(t->end-t->start)==n&&!memcmp(d->bytes+t->start,name,n))return (int)k+1;k=d->token[k+1].next;}return -1;
}
int metadata_json_index(const MetadataJson*d,int parent,unsigned index){
 if(parent<0||(unsigned)parent>=d->count||d->token[parent].type!=MJ_ARRAY||index>=d->token[parent].count)return -1;
 unsigned k=(unsigned)parent+1;while(index--)k=d->token[k].next;return (int)k;
}
bool metadata_json_string(const MetadataJson*d,int i,char*out,size_t cap){
 if(i<0||(unsigned)i>=d->count||d->token[i].type!=MJ_STRING)return false;
 const MetadataJsonToken*t=&d->token[i];size_t n=t->end-t->start;if(n>=cap||memchr(d->bytes+t->start,'\\',n))return false;
 memcpy(out,d->bytes+t->start,n);out[n]=0;return true;
}
bool metadata_json_is(const MetadataJson*d,int i,const char*s){
 if(i<0||(unsigned)i>=d->count)return false;
 const MetadataJsonToken*t=&d->token[i];return strlen(s)==(size_t)(t->end-t->start)&&!memcmp(d->bytes+t->start,s,strlen(s));
}
static bool decimal(const MetadataJson*d,int i,uint8_t type,bool negative,uint64_t*out){
 if(i<0||(unsigned)i>=d->count||d->token[i].type!=type)return false;
 size_t first=d->token[i].start,end=d->token[i].end;if(first==end)return false;
 if(negative){if(d->bytes[first++]!='-'||first==end)return false;}
 if(end-first>1&&d->bytes[first]=='0')return false;
 uint64_t n=0;for(size_t k=first;k<end;k++){uint8_t c=d->bytes[k];if(c<'0'||c>'9'||n>(UINT64_MAX-(c-'0'))/10)return false;n=n*10+(c-'0');}*out=n;return true;
}
bool metadata_json_u64(const MetadataJson*d,int i,uint64_t*out){return decimal(d,i,MJ_STRING,false,out);}
bool metadata_json_i64(const MetadataJson*d,int i,int64_t*out){
 if(i<0||(unsigned)i>=d->count)return false;
 bool negative=d->token[i].start<d->token[i].end&&d->bytes[d->token[i].start]=='-';uint64_t n;
 if(!decimal(d,i,MJ_STRING,negative,&n)||n>(negative?(UINT64_C(1)<<63):(uint64_t)INT64_MAX)||(negative&&!n))return false;
 *out=negative?(n==(UINT64_C(1)<<63)?INT64_MIN:-(int64_t)n):(int64_t)n;return true;
}
bool metadata_json_u32(const MetadataJson*d,int i,uint32_t*out){uint64_t n;if(!decimal(d,i,MJ_NUMBER,false,&n)||n>UINT32_MAX)return false;*out=(uint32_t)n;return true;}
bool metadata_json_double(const MetadataJson*d,int i,double*out){
 if(i<0||(unsigned)i>=d->count||d->token[i].type!=MJ_NUMBER)return false;
 size_t n=d->token[i].end-d->token[i].start;char b[64];if(n>=sizeof(b))return false;memcpy(b,d->bytes+d->token[i].start,n);b[n]=0;char*end;errno=0;double v=strtod(b,&end);if(errno||end!=b+n||!isfinite(v))return false;*out=v;return true;
}
bool metadata_json_bool(const MetadataJson*d,int i,bool*out){
 if(i<0||(unsigned)i>=d->count||d->token[i].type!=MJ_LITERAL)return false;
 if(metadata_json_is(d,i,"true")){*out=true;return true;}if(metadata_json_is(d,i,"false")){*out=false;return true;}return false;
}
