#ifndef LFN_NETWORK_EXTRA_H
#define LFN_NETWORK_EXTRA_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#define NETWORK_REPORT_BYTES 98304u
typedef struct {
 void *context;
 int (*marker)(void *,unsigned,const char *,size_t,uint64_t *);
 int (*report)(void *,const char *,const char *,char *,size_t);
} NetworkExtraHooks;
typedef struct {
 atomic_flag lock;
 NetworkExtraHooks hooks;
 struct {uint64_t request,id;unsigned strength;char note[513];atomic_int state;int error;} marker;
 struct {uint64_t request;char path[64],compare[64];atomic_int state;int error;char data[NETWORK_REPORT_BYTES];} report;
} NetworkExtra;
/* state: 0 idle, 1 pending, 2 worker owns, 3 completed, 4 failed.
 * Only the HTTP task calls request; capture and uartcmd call their own poll. */
void network_extra_init(NetworkExtra *,const NetworkExtraHooks *);
int network_extra_request(NetworkExtra *,const char *,const char *,const char *,size_t,char *,size_t);
void network_extra_capture_step(NetworkExtra *);
void network_extra_io_step(NetworkExtra *);
bool network_extra_busy(NetworkExtra *);
int lfn_network_extra_request(const char *,const char *,const char *,size_t,char *,size_t);
void lfn_image_network_extra_init(void);
void lfn_image_network_extra_capture_step(void);
void lfn_image_network_extra_io_step(void);
bool lfn_image_network_extra_busy(void);
#endif
