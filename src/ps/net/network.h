#ifndef LFN_NETWORK_H
#define LFN_NETWORK_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef bool (*lfn_network_json_fn)(const char *, char *, size_t);
typedef int (*lfn_network_control_fn)(const char *method,const char *path,const char *body,size_t bytes,char *out,size_t capacity);
typedef bool (*lfn_network_asset_fn)(const char *, const char **, size_t *, const char **);
typedef void (*lfn_network_log_fn)(const char *);
typedef struct {
    uintptr_t gem_base;
    const char *ip, *mask, *gateway;
    uint8_t mac[6];
    bool dhcp;
    lfn_network_json_fn json;
    lfn_network_asset_fn asset;
    lfn_network_log_fn log;
    lfn_network_control_fn control;
} lfn_network_config_t;
/* Once only, from a running task; config strings must remain valid.
 * JSON callback copies RAM snapshots and NUL-terminates; no SD I/O or network
 * calls. The server serializes one connection at a time and owns its buffers. */
bool lfn_network_start(const lfn_network_config_t *config);
/* Diagnostic sink is a queued UART owner callback; never direct FIFO writes. */
void lfn_network_log(const char *format, ...);
#endif
