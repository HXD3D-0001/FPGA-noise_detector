#ifndef LFN_HTTP_REQUEST_H
#define LFN_HTTP_REQUEST_H
#include <stddef.h>
#define LFN_HTTP_HEADER_LIMIT 2048u
#define LFN_HTTP_PATH_CAPACITY 96u
#define LFN_HTTP_BODY_LIMIT 2048u
#define LFN_HTTP_REQUEST_LIMIT (LFN_HTTP_HEADER_LIMIT+LFN_HTTP_BODY_LIMIT)
typedef struct {char method[5],path[LFN_HTTP_PATH_CAPACITY];size_t body_offset,body_bytes;} LfnHttpRequest;
/* Header and fixed Content-Length body, bounded independently. Only GET/POST;
 * POST rejects browser Origin and transfer-encoding, preventing form writes. */
int lfn_http_parse(const char *,size_t,LfnHttpRequest *);
/* 0: incomplete; 200: GET accepted; otherwise the HTTP error status. */
int lfn_http_request(const char *bytes, size_t length,
                     char path[LFN_HTTP_PATH_CAPACITY]);
#endif
