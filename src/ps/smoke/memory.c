#include <stddef.h>

/* Compile with -ffreestanding -fno-builtin: never depend on external libc. */
void *memset(void *dst,int value,size_t count) {
    volatile unsigned char *d=(volatile unsigned char *)dst;
    while(count--) *d++=(unsigned char)value;
    return dst;
}
void *memcpy(void *dst,const void *src,size_t count) {
    volatile unsigned char *d=(volatile unsigned char *)dst;
    const volatile unsigned char *s=(const volatile unsigned char *)src;
    while(count--) *d++=*s++;
    return dst;
}
void *memmove(void *dst,const void *src,size_t count) {
    volatile unsigned char *d=(volatile unsigned char *)dst;
    const volatile unsigned char *s=(const volatile unsigned char *)src;
    if((size_t)dst<(size_t)src) while(count--) *d++=*s++;
    else {d+=count;s+=count;while(count--) *--d=*--s;}
    return dst;
}
int memcmp(const void *a,const void *b,size_t count) {
    const unsigned char *x=(const unsigned char *)a,*y=(const unsigned char *)b;
    while(count--) {if(*x!=*y) return (int)*x-(int)*y;++x;++y;}
    return 0;
}
void __aeabi_memcpy(void *d,const void *s,size_t n) {(void)memcpy(d,s,n);}
void __aeabi_memcpy4(void *d,const void *s,size_t n) {(void)memcpy(d,s,n);}
void __aeabi_memcpy8(void *d,const void *s,size_t n) {(void)memcpy(d,s,n);}
void __aeabi_memset(void *d,size_t n,int c) {(void)memset(d,c,n);}
void __aeabi_memset4(void *d,size_t n,int c) {(void)memset(d,c,n);}
void __aeabi_memset8(void *d,size_t n,int c) {(void)memset(d,c,n);}
void __aeabi_memclr(void *d,size_t n) {(void)memset(d,0,n);}
void __aeabi_memclr4(void *d,size_t n) {(void)memset(d,0,n);}
void __aeabi_memclr8(void *d,size_t n) {(void)memset(d,0,n);}
