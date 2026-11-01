#include "http_request.h"
#include <stdbool.h>
#include <string.h>

static bool header_is(const char*s,size_t n,const char*key){
 size_t len=strlen(key);if(n<len)return false;
 for(size_t i=0;i<len;i++){unsigned char ch=(unsigned char)s[i];if(ch>='A'&&ch<='Z')ch=(unsigned char)(ch+'a'-'A');if(ch!=(unsigned char)key[i])return false;}
 return true;
}
int lfn_http_parse(const char *bytes,size_t length,LfnHttpRequest *out){
 if(!bytes||!out)return 400;
 size_t end=0,line=0;
 for(size_t i=0;i<length;i++){
  if(!bytes[i])return 400;
  if(i+3<length&&!memcmp(bytes+i,"\r\n\r\n",4)){end=i+4;break;}
 }
 if(!end)return length>=LFN_HTTP_HEADER_LIMIT?431:0;
 if(end>LFN_HTTP_HEADER_LIMIT)return 431;
 while(line+1<end&&memcmp(bytes+line,"\r\n",2))line++;
 size_t offset;
 if(line>=4&&!memcmp(bytes,"GET ",4)){offset=4;strcpy(out->method,"GET");}
 else if(line>=5&&!memcmp(bytes,"POST ",5)){offset=5;strcpy(out->method,"POST");}
 else return 405;
 size_t space=offset;while(space<line&&bytes[space]!=' ')space++;
 size_t n=space-offset;
 if(!n||n>=LFN_HTTP_PATH_CAPACITY||bytes[offset]!='/'||line-space!=9||
    (memcmp(bytes+space," HTTP/1.0",9)&&memcmp(bytes+space," HTTP/1.1",9)))return 400;
 for(size_t i=offset;i<space;i++)if((unsigned char)bytes[i]<=32||(unsigned char)bytes[i]>=127)return 400;
 memcpy(out->path,bytes+offset,n);out->path[n]=0;
 bool have_length=false;size_t count=0;
 for(size_t at=line+2;at+2<end;){
  size_t stop=at;while(stop+1<end&&memcmp(bytes+stop,"\r\n",2))stop++;
  if(stop==at)break;
  if(header_is(bytes+at,stop-at,"content-length:")){
   if(have_length)return 400;
   have_length=true;size_t k=at+15;while(k<stop&&(bytes[k]==' '||bytes[k]=='\t'))k++;
   if(k==stop||bytes[k]<'0'||bytes[k]>'9')return 400;
   while(k<stop&&bytes[k]>='0'&&bytes[k]<='9'){
    count=count*10+(unsigned)(bytes[k++]-'0');if(count>LFN_HTTP_BODY_LIMIT)return 413;
   }
   while(k<stop&&(bytes[k]==' '||bytes[k]=='\t'))k++;
   if(k!=stop)return 400;
  }
  if(header_is(bytes+at,stop-at,"transfer-encoding:"))return 400;
  if(offset==5&&header_is(bytes+at,stop-at,"origin:"))return 403;
  at=stop+2;
 }
 if(offset==4&&count)return 400;
 if(length<end+count)return 0;
 if(length!=end+count)return 400;
 for(size_t i=end;i<length;i++)if(!bytes[i])return 400;
 out->body_offset=end;out->body_bytes=count;return 200;
}

int lfn_http_request(const char *bytes, size_t length,
                     char path[LFN_HTTP_PATH_CAPACITY])
{
    size_t end = 0, line = 0, space, n;
    for (size_t i = 0; i < length; ++i) {
        if (bytes[i] == '\0') return 400;
        if (i + 3 < length && memcmp(bytes + i, "\r\n\r\n", 4) == 0) {
            end = i + 4;
            break;
        }
    }
    if (!end) return length >= LFN_HTTP_HEADER_LIMIT ? 431 : 0;
    if (end > LFN_HTTP_HEADER_LIMIT) return 431;
    while (line + 1 < end && memcmp(bytes + line, "\r\n", 2)) ++line;
    if (line < 14) return 400;
    if (memcmp(bytes, "GET ", 4)) return 405;
    space = 4;
    while (space < line && bytes[space] != ' ') ++space;
    n = space - 4;
    if (!n || n >= LFN_HTTP_PATH_CAPACITY || bytes[4] != '/' ||
        line - space != 9 ||
        (memcmp(bytes + space, " HTTP/1.0", 9) &&
         memcmp(bytes + space, " HTTP/1.1", 9))) return 400;
    for (size_t i = 4; i < space; ++i)
        if ((unsigned char)bytes[i] <= 32 || (unsigned char)bytes[i] >= 127)
            return 400;
    memcpy(path, bytes + 4, n);
    path[n] = '\0';
    return 200;
}
