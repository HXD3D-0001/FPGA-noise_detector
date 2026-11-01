#include "network.h"
#include "http_request.h"
#include "ksz9031.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "lwip/init.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "lwip/tcpip.h"
#include "lwip/dhcp.h"
#include "netif/xadapter.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>

#if NO_SYS || !LWIP_SOCKET || !LWIP_SOCKET_SELECT
#error Network requires RTOS sockets and select support
#endif
#define HTTP_TIMEOUT_MS 3000u
#define RESPONSE_CAPACITY 98304u
#define DISCOVERY_PORT 37021
static lfn_network_config_t settings;
static struct netif interface;
static char request[LFN_HTTP_REQUEST_LIMIT];
static char response[RESPONSE_CAPACITY];
static bool started;

void lfn_network_log(const char *format, ...)
{
    char text[192];
    va_list args;
    va_start(args, format);
    (void)vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    size_t length = strlen(text);
    while (length && (text[length - 1] == '\r' || text[length - 1] == '\n'))
        text[--length] = '\0';
    if (settings.log) settings.log(text);
}

typedef struct {
    ip4_addr_t ip, mask, gateway;
    SemaphoreHandle_t done;
    bool fallback;
} core_config_t;

static void configure_core(void *argument)
{
    core_config_t *config = argument;
    netif_set_default(&interface);
    netif_set_up(&interface);
#if LWIP_DHCP
    if (settings.dhcp) {
        if (config->fallback) {
            dhcp_stop(&interface);
            netif_set_addr(&interface, &config->ip, &config->mask, &config->gateway);
        } else {
            netif_set_addr(&interface, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4);
            (void)dhcp_start(&interface);
        }
    }
#endif
    xSemaphoreGive(config->done);
}

static bool configure_wait(core_config_t *config)
{
    if (tcpip_callback(configure_core, config) != ERR_OK) return false;
    return xSemaphoreTake(config->done, portMAX_DELAY) == pdTRUE;
}

static bool socket_ready(int fd, bool write, TickType_t start)
{
    fd_set set;
    TickType_t elapsed = xTaskGetTickCount() - start;
    TickType_t limit = pdMS_TO_TICKS(HTTP_TIMEOUT_MS);
    if (elapsed >= limit) return false;
    unsigned ms = (unsigned)((limit - elapsed) * portTICK_PERIOD_MS);
    struct timeval timeout = {(long)(ms / 1000u), (long)((ms % 1000u) * 1000u)};
    FD_ZERO(&set);
    FD_SET(fd, &set);
    return lwip_select(fd + 1, write ? NULL : &set, write ? &set : NULL,
                       NULL, &timeout) > 0;
}

static bool send_bytes(int fd, const char *data, size_t length, TickType_t start)
{
    while (length) {
        if (!socket_ready(fd, true, start)) return false;
        size_t chunk = length > 1460u ? 1460u : length;
        int n = lwip_send(fd, data, chunk, 0);
        if (n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) continue;
        if (n <= 0) return false;
        data += n;
        length -= (size_t)n;
    }
    return true;
}

static void serve(int fd)
{
    LfnHttpRequest parsed;
    char header[256];
    int status = 0;
    size_t used = 0, length;
    const char *body, *type = "application/json; charset=utf-8";
    const char *reason;
    TickType_t start = xTaskGetTickCount();
    int nonblocking = 1;
    if (lwip_ioctl(fd, FIONBIO, &nonblocking)) return;
    while (!status && xTaskGetTickCount() - start < pdMS_TO_TICKS(HTTP_TIMEOUT_MS)) {
        if (!socket_ready(fd, false, start)) break;
        int n = lwip_recv(fd, request + used, sizeof(request) - used, 0);
        if (n <= 0) {
            if (n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) continue;
            return;
        }
        used += (size_t)n;
        status = lfn_http_parse(request, used, &parsed);
    }
    if (!status) status = 408;
    body = "{\"error\":\"request_rejected\"}";
    length = strlen(body);
    if (status == 200) {
        const char *path=parsed.path;
        if (!strcmp(parsed.method,"GET")&&(!strcmp(path, "/api/status") || !strcmp(path, "/api/spectrum"))) {
            response[0] = '\0';
            if (settings.json && settings.json(path, response, sizeof(response)) &&
                memchr(response, '\0', sizeof(response))) {
                body = response;
                length = strlen(body);
            } else {
                status = 503;
                body = "{\"error\":\"not_ready\"}";
                length = strlen(body);
            }
        } else {
            response[0]='\0';
            status=settings.control?settings.control(parsed.method,path,request+parsed.body_offset,parsed.body_bytes,response,sizeof(response)):404;
            if(status==404&&!strcmp(parsed.method,"GET")&&settings.asset&&settings.asset(path,&body,&length,&type))status=200;
            else if(response[0]&&memchr(response,'\0',sizeof(response))){body=response;length=strlen(body);}
            else{body="{\"error\":\"request_rejected\"}";length=strlen(body);}
        }
    }
    switch (status) {
    case 200: reason = "OK"; break;
    case 202: reason = "Accepted"; break;
    case 403: reason = "Forbidden"; break;
    case 404: reason = "Not Found"; break;
    case 405: reason = "Method Not Allowed"; break;
    case 408: reason = "Request Timeout"; break;
    case 409: reason = "Conflict"; break;
    case 413: reason = "Payload Too Large"; break;
    case 431: reason = "Request Header Fields Too Large"; break;
    case 503: reason = "Service Unavailable"; break;
    default: reason = "Bad Request"; break;
    }
    int n = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %lu\r\n"
        "Connection: close\r\nCache-Control: no-store\r\n\r\n",
        status, reason, type, (unsigned long)length);
    start = xTaskGetTickCount();
    if (n > 0 && (size_t)n < sizeof(header) && send_bytes(fd, header, (size_t)n, start))
        (void)send_bytes(fd, body, length, start);
}

static void answer_discovery(int fd)
{
    static const char prefix[] = "AX7021_DISCOVER_V1 ";
    /* One extra byte detects oversized datagrams, including truncated ones. */
    char query[sizeof(prefix) + 16], reply[64];
    struct sockaddr_in peer;
    socklen_t peer_size = sizeof(peer);
    int count = lwip_recvfrom(fd, query, sizeof(query), 0,
                              (struct sockaddr *)&peer, &peer_size);
    if (count != (int)sizeof(query) - 1 || memcmp(query, prefix, sizeof(prefix) - 1))
        return;
    char *nonce = query + sizeof(prefix) - 1;
    for (unsigned i = 0; i < 16; i++) {
        char c = nonce[i];
        if (!(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'f') && !(c >= 'A' && c <= 'F'))
            return;
    }
    const uint8_t *mac = settings.mac;
    int n = snprintf(reply, sizeof(reply), "AX7021_MONITOR_V1 %.16s %02x%02x%02x%02x%02x%02x",
                     nonce, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (n > 0 && (size_t)n < sizeof(reply))
        (void)lwip_sendto(fd, reply, (size_t)n, 0, (struct sockaddr *)&peer, peer_size);
}

static void http_thread(void *unused)
{
    (void)unused;
    /* JSON report rendering uses floating point; preserve the task's VFP state. */
    portTASK_USES_FLOATING_POINT();
    struct sockaddr_in addr;
    int listener = lwip_socket(AF_INET, SOCK_STREAM, 0);
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(80);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (listener < 0 || lwip_bind(listener, (struct sockaddr *)&addr, sizeof(addr)) ||
        lwip_listen(listener, 1)) {
        lfn_network_log("NET HTTP_START_FAILED\r\n");
        if (listener >= 0) lwip_close(listener);
        vTaskDelete(NULL);
        return;
    }
    lfn_network_log("NET HTTP_READY port=80 connections=1 header=2048 timeout_ms=3000\r\n");
    int discovery = lwip_socket(AF_INET, SOCK_DGRAM, 0);
    addr.sin_port = htons(DISCOVERY_PORT);
    if (discovery >= 0 && lwip_bind(discovery, (struct sockaddr *)&addr, sizeof(addr))) {
        lwip_close(discovery);
        discovery = -1;
    }
    lfn_network_log(discovery < 0 ? "NET DISCOVERY_START_FAILED" : "NET DISCOVERY_READY port=37021");
    for (;;) {
        fd_set readers;
        FD_ZERO(&readers);
        FD_SET(listener, &readers);
        if (discovery >= 0) FD_SET(discovery, &readers);
        int highest = listener > discovery ? listener : discovery;
        if (lwip_select(highest + 1, &readers, NULL, NULL, NULL) <= 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (discovery >= 0 && FD_ISSET(discovery, &readers)) answer_discovery(discovery);
        if (!FD_ISSET(listener, &readers)) continue;
        int fd = lwip_accept(listener, NULL, NULL);
        if (fd >= 0) {
            serve(fd);
            lwip_close(fd);
        } else vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void network_thread(void *unused)
{
    ip4_addr_t ip, mask, gateway;
    core_config_t core;
    (void)unused;
    if (!ip4addr_aton(settings.ip, &ip) || !ip4addr_aton(settings.mask, &mask) ||
        !ip4addr_aton(settings.gateway, &gateway)) {
        lfn_network_log("NET INVALID_IPV4\r\n");
        vTaskDelete(NULL);
        return;
    }
    /* AMD's lwip_init starts tcpip_thread and waits for its ready callback. */
    lwip_init();
    lfn_phy_initialize();
    if (!xemac_add(&interface, &ip, &mask, &gateway, settings.mac, settings.gem_base)) {
        lfn_network_log("NET GEM_INIT_FAILED\r\n");
        vTaskDelete(NULL);
        return;
    }
    core.ip = ip; core.mask = mask; core.gateway = gateway;
    core.fallback = false;
    core.done = xSemaphoreCreateBinary();
    if (!core.done || !configure_wait(&core)) {
        lfn_network_log("NET CORE_CONFIG_FAILED\r\n");
        if (core.done) vSemaphoreDelete(core.done);
        vTaskDelete(NULL);
        return;
    }
    if (!sys_thread_new("net_rx", (void (*)(void *))xemacif_input_thread,
                        &interface, 1024, 3)) {
        lfn_network_log("NET RX_TASK_FAILED\r\n");
        vSemaphoreDelete(core.done);
        vTaskDelete(NULL);
        return;
    }
#if LWIP_DHCP
    if (settings.dhcp) {
        /* lwIP core timers maintain DHCP; do not call DHCP timers twice. */
        for (unsigned i = 0; i < 100 && ip4_addr_isany_val(*netif_ip4_addr(&interface)); ++i)
            vTaskDelay(pdMS_TO_TICKS(100));
        if (ip4_addr_isany_val(*netif_ip4_addr(&interface))) {
            core.fallback = true;
            (void)configure_wait(&core);
            lfn_network_log("NET DHCP_TIMEOUT static_fallback\r\n");
        }
    }
#endif
    vSemaphoreDelete(core.done);
    lfn_network_log("NET IP=%s\r\n", ip4addr_ntoa(netif_ip4_addr(&interface)));
    if (!sys_thread_new("http", http_thread, NULL, 2048, 1))
        lfn_network_log("NET HTTP_TASK_FAILED\r\n");
    vTaskDelete(NULL);
}

bool lfn_network_start(const lfn_network_config_t *config)
{
    if (started || !config || !config->ip || !config->mask || !config->gateway) return false;
    settings = *config;
    started = sys_thread_new("net_start", network_thread, NULL, 2048, 1) != NULL;
    return started;
}
