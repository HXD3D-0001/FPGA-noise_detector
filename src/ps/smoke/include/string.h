#ifndef LFN_SMOKE_STRING_H
#define LFN_SMOKE_STRING_H
#include <stddef.h>
void *memset(void *dst,int value,size_t count);
void *memcpy(void *dst,const void *src,size_t count);
void *memmove(void *dst,const void *src,size_t count);
int memcmp(const void *a,const void *b,size_t count);
#endif
