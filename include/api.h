#ifndef ATP_API_H
#define ATP_API_H

#include "atp.h"
#include "reactor.h"
#include <sys/types.h>
#include <time.h>
#include <netdb.h>

#define API_MAX_HOST_LEN 255
#define API_MAX_HEADER_SIZE 32768
#define API_MAX_RESPONSE_SIZE (8 * 1024 * 1024)
#define API_MAX_PENDING_REQUESTS 1024
#define API_CHUNK_READ_BUFFER 8192

/* HTTP parse states */
typedef enum {
    HTTP_PARSE_HEADERS = 0,
    HTTP_PARSE_BODY_CONTENT_LENGTH,
    HTTP_PARSE_BODY_CHUNKED,
    HTTP_PARSE_BODY_CLOSE,
    HTTP_PARSE_DONE
} http_parse_state_t;

/* Chunk decoder states */
typedef enum {
    CHUNK_READ_SIZE = 0,
    CHUNK_READ_DATA,
    CHUNK_READ_CRLF,
    CHUNK_READ_TRAILER,
    CHUNK_DONE
} chunk_state_t;

typedef enum {
    API_MODE_RULE = 0,
    API_MODE_GLOBAL,
    API_MODE_DIRECT,
    API_MODE_GOOGLE_VPN
} api_mode_t;

typedef enum {
    API_STATE_IDLE = 0,
    API_STATE_CONNECTING,
    API_STATE_SENDING,
    API_STATE_RECEIVING,
    API_STATE_DONE,
    API_STATE_ERROR
} api_state_t;

typedef struct api_request_s {
    struct api_ctx_s *ctx;

    char method[16];
    char path[256];
    char host[64];
    int port;

    int sock_fd;

    char *body;
    char *send_buf;
    size_t send_len;
    size_t send_offset;

    char *recv_buf;
    size_t recv_size;
    size_t recv_offset;

    /* Decoded body for chunked encoding */
    char *decoded_body;
    size_t decoded_body_size;
    size_t decoded_body_len;

    size_t raw_body_received;
    size_t body_received;
    long content_length;
    long bytes_to_read;

    /* HTTP version */
    uint8_t http_major;
    uint8_t http_minor;

    int http_code;
    int headers_complete;
    int keepalive_disabled;

    /* HTTP/1.1 features */
    int chunked_encoding;
    int read_until_close;
    http_parse_state_t parse_state;

    /* Chunk decoder state */
    chunk_state_t chunk_state;
    size_t chunk_size;
    size_t chunk_offset;
    size_t chunk_parse_offset;
    size_t trailer_received;

    api_state_t state;
    time_t start_time;

    struct addrinfo *addr_info;
    struct addrinfo *current_addr;

    void (*callback)(int http_code, const char *body, void *userdata);
    void *userdata;

    struct api_request_s *next;
} api_request_t;

typedef struct api_ctx_s {
    char base_url[128];
    char secret[128];
    int timeout_sec;

    api_request_t *pending_requests;
    int pending_count;

    int keepalive_fd;
    time_t keepalive_time;
    char keepalive_host[256];
    int keepalive_port;

    int last_http_code;
    char last_error[256];
} api_ctx_t;

typedef void (*api_callback_t)(int http_code, const char *body, void *userdata);

int api_init(api_ctx_t *ctx, atp_config_t *cfg);
void api_cleanup(api_ctx_t *ctx);

int api_start_with_reactor(api_ctx_t *ctx, reactor_t *r);

int api_get_mode_async(api_ctx_t *ctx, api_callback_t callback, void *userdata);
int api_set_mode_async(api_ctx_t *ctx, const char *mode, api_callback_t callback, void *userdata);
int api_check_health_async(api_ctx_t *ctx, api_callback_t callback, void *userdata);
int api_get_proxies_async(api_ctx_t *ctx, api_callback_t callback, void *userdata);
int api_request_raw_async(api_ctx_t *ctx, const char *method, const char *url,
                          const char *body, api_callback_t callback, void *userdata);

int api_get_mode_sync(api_ctx_t *ctx, char *mode, size_t size);
int api_get_mode(api_ctx_t *ctx, char *mode, size_t size);
int api_get_sync(const char *url, char *response, size_t response_size);

const char *api_mode_to_string(api_mode_t mode);
api_mode_t api_string_to_mode(const char *str);

int api_get_fds(api_ctx_t *ctx, int *fds, int max_fds);
int api_handle_event(api_ctx_t *ctx, int fd, int events);
int api_process(api_ctx_t *ctx);

#endif
