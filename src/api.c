#include "atpd_global.h"
/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * Clash API client - Reactor-driven async state machine
 * RFC 7230/7231 compliant HTTP/1.1 client
 * Supports: Content-Length, chunked transfer encoding, read-until-close
 */

#include "api.h"
#include "logger.h"
#include "utils.h"
#include "reactor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <strings.h>
#include <yyjson.h>
#include <netdb.h>
#include <limits.h>
#include <sys/socket.h>

#define API_MAX_HOST_LEN 255
#define API_MAX_HEADER_SIZE 32768
#define API_MAX_RESPONSE_SIZE (8 * 1024 * 1024)
#define API_MAX_PENDING_REQUESTS 1024
#define API_CHUNK_READ_BUFFER 8192


static reactor_t *g_api_reactor = NULL;

static int api_parse_url(const char *base_url, char *host, int *port);
static int api_parse_full_url(const char *url, char *host, int *port, char *path, size_t path_size);
static int api_build_http_request(api_request_t *req);
static void api_request_cleanup(api_request_t *req);
static int api_socket_connect(api_request_t *req);
static int api_parse_headers(api_request_t *req);
static int api_decode_chunked(api_request_t *req);
static const char *api_extract_body(api_request_t *req);
static void api_io_callback(reactor_t *r, int fd, uint32_t events, void *userdata);
static void api_process_requests(api_ctx_t *ctx);
static int api_validate_request(api_request_t *req);
static int api_socket_alive(int fd);
static int api_parse_status_line(const char *line, int *code);
static int api_parse_header_line(const char *line, size_t len, char **name, char **value);
static void *api_memmem(const void *haystack, size_t haystack_len,
                         const void *needle, size_t needle_len);

static void *api_memmem(const void *haystack, size_t haystack_len,
                         const void *needle, size_t needle_len) {
    if (!haystack || !needle || needle_len == 0 || haystack_len < needle_len) {
        return NULL;
    }

    const unsigned char *h = (const unsigned char *)haystack;
    const unsigned char *n = (const unsigned char *)needle;

    for (size_t i = 0; i <= haystack_len - needle_len; i++) {
        if (memcmp(h + i, n, needle_len) == 0) {
            return (void *)(h + i);
        }
    }

    return NULL;
}

static void safe_str_copy(char *dest, size_t dest_size, const char *src, size_t src_len) {
    if (!dest || dest_size == 0 || !src) return;

    if (src_len >= dest_size) {
        src_len = dest_size - 1;
    }

    memcpy(dest, src, src_len);
    dest[src_len] = '\0';
}

static int validate_host_port(const char *host, int port) {
    if (!host || !*host) return -1;
    if (strlen(host) > API_MAX_HOST_LEN) return -1;
    if (port < 1 || port > 65535) return -1;
    if (strchr(host, '\r') || strchr(host, '\n')) return -1;
    return 0;
}

static int api_validate_request(api_request_t *req) {
    if (!req) return -1;

    if (!req->method[0]) {
        LOG_ERROR("API: empty method");
        return -1;
    }

    if (!req->path[0]) {
        LOG_ERROR("API: empty path");
        return -1;
    }

    if (strchr(req->path, '\r') || strchr(req->path, '\n')) {
        LOG_ERROR("API: invalid path contains CR/LF");
        return -1;
    }

    if (strchr(req->host, '\r') || strchr(req->host, '\n')) {
        LOG_ERROR("API: invalid host contains CR/LF");
        return -1;
    }

    if (strlen(req->host) > API_MAX_HOST_LEN) {
        LOG_ERROR("API: host too long");
        return -1;
    }

    if (req->port < 1 || req->port > 65535) {
        LOG_ERROR("API: invalid port %d", req->port);
        return -1;
    }

    return 0;
}

static int api_parse_url(const char *base_url, char *host, int *port) {
    const char *start;

    if (strncmp(base_url, "http://", 7) == 0) {
        start = base_url + 7;
        *port = 80;
    } else if (strncmp(base_url, "https://", 8) == 0) {
        LOG_ERROR("API: HTTPS not supported");
        return -1;
    } else {
        start = base_url;
        *port = 80;
    }

    const char *colon = strchr(start, ':');
    const char *slash = strchr(start, '/');

    if (colon && (!slash || colon < slash)) {
        size_t len = colon - start;
        safe_str_copy(host, 64, start, len);
        char *endptr;
        long p = strtol(colon + 1, &endptr, 10);
        if (endptr != colon + 1 && p >= 1 && p <= 65535) {
            *port = (int)p;
        } else {
            *port = 80;
        }
    } else if (slash) {
        size_t len = slash - start;
        safe_str_copy(host, 64, start, len);
    } else {
        safe_str_copy(host, 64, start, strlen(start));
    }

    return 0;
}

static int api_parse_full_url(const char *url, char *host, int *port, char *path, size_t path_size) {
    const char *start;

    if (strncmp(url, "https://", 8) == 0) {
        LOG_ERROR("API: HTTPS not supported for raw requests");
        return -1;
    }

    if (strncmp(url, "http://", 7) == 0) {
        start = url + 7;
        *port = 80;
    } else {
        start = url;
        *port = 80;
    }

    const char *slash = strchr(start, '/');
    const char *colon = strchr(start, ':');

    if (colon && (!slash || colon < slash)) {
        size_t len = colon - start;
        safe_str_copy(host, 64, start, len);
        char *endptr;
        long p = strtol(colon + 1, &endptr, 10);
        if (endptr != colon + 1 && p >= 1 && p <= 65535) {
            *port = (int)p;
        } else {
            *port = 80;
        }
        if (slash) {
            safe_str_copy(path, path_size, slash, strlen(slash));
        } else {
            safe_str_copy(path, path_size, "/", 1);
        }
    } else if (slash) {
        size_t len = slash - start;
        safe_str_copy(host, 64, start, len);
        safe_str_copy(path, path_size, slash, strlen(slash));
    } else {
        safe_str_copy(host, 64, start, strlen(start));
        safe_str_copy(path, path_size, "/", 1);
    }

    return 0;
}
int api_init(api_ctx_t *ctx, atp_config_t *cfg) {
    memset(ctx, 0, sizeof(api_ctx_t));

    snprintf(ctx->base_url, sizeof(ctx->base_url), "http://%s:%d",
             cfg->api.host, cfg->api.port);

    if (cfg->filter.clash_secret[0] != '\0') {
        snprintf(ctx->secret, sizeof(ctx->secret), "%s", cfg->filter.clash_secret);
    }

    ctx->timeout_sec = 2;
    ctx->pending_requests = NULL;
    ctx->keepalive_fd = -1;
    ctx->keepalive_time = 0;
    ctx->keepalive_host[0] = '\0';
    ctx->keepalive_port = 0;
    ctx->pending_count = 0;

    LOG_INFO("API initialized (Reactor-driven + keep-alive): %s", ctx->base_url);
    return 0;
}

void api_cleanup(api_ctx_t *ctx) {
    api_request_t *req = ctx->pending_requests;
    while (req) {
        api_request_t *next = req->next;
        if (req->sock_fd >= 0 && g_api_reactor) {
            reactor_remove_fd(g_api_reactor, req->sock_fd);
        }
        api_request_cleanup(req);
        req = next;
    }
    ctx->pending_requests = NULL;
    ctx->pending_count = 0;

    if (ctx->keepalive_fd >= 0) {
        if (g_api_reactor) {
            reactor_remove_fd(g_api_reactor, ctx->keepalive_fd);
        }
        close(ctx->keepalive_fd);
        ctx->keepalive_fd = -1;
    }
    ctx->keepalive_host[0] = '\0';
    ctx->keepalive_port = 0;

    g_api_reactor = NULL;
}

static int api_build_http_request(api_request_t *req) {
    char headers[4096];
    size_t used = 0;
    size_t remain = sizeof(headers);
    int ret;

    ret = snprintf(headers + used, remain,
                   "%s %s HTTP/1.1\r\n"
                   "Host: %s\r\n"
                   "User-Agent: ATPd/1.0\r\n"
                   "Accept: application/json\r\n"
                   "Connection: keep-alive\r\n",
                   req->method, req->path, req->host);

    if (ret < 0) return -1;
    if ((size_t)ret >= remain) {
        LOG_ERROR("API: headers too large (host/path too long)");
        return -1;
    }
    used += ret;
    remain -= ret;

    if (req->ctx->secret[0]) {
        ret = snprintf(headers + used, remain,
                       "Authorization: Bearer %s\r\n", req->ctx->secret);
        if (ret < 0) return -1;
        if ((size_t)ret >= remain) {
            LOG_ERROR("API: headers too large (secret too long)");
            return -1;
        }
        used += ret;
        remain -= ret;
    }

    if (req->body) {
        size_t body_len = strlen(req->body);
        ret = snprintf(headers + used, remain,
                       "Content-Type: application/json\r\n"
                       "Content-Length: %zu\r\n", body_len);
        if (ret < 0) return -1;
        if ((size_t)ret >= remain) {
            LOG_ERROR("API: headers too large (body length)");
            return -1;
        }
        used += ret;
        remain -= ret;
    }

    ret = snprintf(headers + used, remain, "\r\n");
    if (ret < 0) return -1;
    if ((size_t)ret >= remain) {
        LOG_ERROR("API: headers too large (final CRLF)");
        return -1;
    }
    used += ret;

    size_t body_len = req->body ? strlen(req->body) : 0;
    req->send_buf = malloc(used + body_len + 1);
    if (!req->send_buf) {
        LOG_ERROR("API: malloc send_buf failed");
        return -1;
    }

    memcpy(req->send_buf, headers, used);
    if (req->body) {
        memcpy(req->send_buf + used, req->body, body_len);
    }

    req->send_len = used + body_len;
    req->send_offset = 0;

    return 0;
}

static void api_request_cleanup(api_request_t *req) {
    if (!req) return;

    if (req->sock_fd >= 0) {
        if (g_api_reactor) {
            reactor_remove_fd(g_api_reactor, req->sock_fd);
        }
        close(req->sock_fd);
        req->sock_fd = -1;
    }

    if (req->addr_info) {
        freeaddrinfo(req->addr_info);
        req->addr_info = NULL;
    }

    free(req->body);
    free(req->send_buf);
    free(req->recv_buf);
    free(req->decoded_body);

    req->body = NULL;
    req->send_buf = NULL;
    req->recv_buf = NULL;
    req->decoded_body = NULL;

    free(req);
}

static int api_socket_alive(int fd) {
    char ch;
    ssize_t n = recv(fd, &ch, 1, MSG_PEEK | MSG_DONTWAIT);

    if (n == 0) {
        return 0;
    }

    if (n > 0) {
        return 1;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return 1;
    }

    return 0;
}

static int api_socket_connect(api_request_t *req) {
    api_ctx_t *ctx = req->ctx;

    if (api_validate_request(req) != 0) {
        LOG_ERROR("API: invalid request");
        return -1;
    }

    if (ctx->keepalive_fd >= 0 &&
        strcmp(req->host, ctx->keepalive_host) == 0 &&
        req->port == ctx->keepalive_port) {
        if (api_socket_alive(ctx->keepalive_fd)) {
            req->sock_fd = ctx->keepalive_fd;
            ctx->keepalive_fd = -1;

            if (g_api_reactor) {
                reactor_remove_fd(g_api_reactor, req->sock_fd);
            }

            req->state = API_STATE_SENDING;
            if (api_build_http_request(req) != 0) {
                close(req->sock_fd);
                req->sock_fd = -1;
                return -1;
            }

            if (g_api_reactor) {
                reactor_add_fd(g_api_reactor, req->sock_fd,
                               REACTOR_EVENT_READ | REACTOR_EVENT_WRITE,
                               api_io_callback, req);
            }

            LOG_DEBUG("API: reused keep-alive connection (fd=%d)", req->sock_fd);
            return 0;
        }

        LOG_DEBUG("API: keep-alive connection dead, reconnecting");
        if (g_api_reactor) {
            reactor_remove_fd(g_api_reactor, ctx->keepalive_fd);
        }
        close(ctx->keepalive_fd);
        ctx->keepalive_fd = -1;
    }

    if (strcmp(req->host, "localhost") == 0 || strcmp(req->host, "127.0.0.1") == 0 ||
        strcmp(req->host, "::1") == 0) {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(req->port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        req->sock_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (req->sock_fd < 0) {
            LOG_ERROR("API: socket failed: %s", strerror(errno));
            return -1;
        }

        if (g_api_reactor) {
            reactor_add_fd(g_api_reactor, req->sock_fd,
                           REACTOR_EVENT_READ | REACTOR_EVENT_WRITE,
                           api_io_callback, req);
        }

        if (connect(req->sock_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            req->state = API_STATE_SENDING;
            if (api_build_http_request(req) != 0) {
                if (g_api_reactor) {
                    reactor_remove_fd(g_api_reactor, req->sock_fd);
                }
                close(req->sock_fd);
                req->sock_fd = -1;
                return -1;
            }
            return 0;
        }

        if (errno == EINPROGRESS) {
            req->state = API_STATE_CONNECTING;
            return 0;
        }

        LOG_ERROR("API: local connect failed: %s", strerror(errno));
        if (g_api_reactor) {
            reactor_remove_fd(g_api_reactor, req->sock_fd);
        }
        close(req->sock_fd);
        req->sock_fd = -1;
        return -1;
    }

    struct sockaddr_in addr;
    if (inet_pton(AF_INET, req->host, &addr.sin_addr) == 1) {
        addr.sin_family = AF_INET;
        addr.sin_port = htons(req->port);

        req->sock_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (req->sock_fd < 0) {
            LOG_ERROR("API: socket failed: %s", strerror(errno));
            return -1;
        }

        if (g_api_reactor) {
            reactor_add_fd(g_api_reactor, req->sock_fd,
                           REACTOR_EVENT_READ | REACTOR_EVENT_WRITE,
                           api_io_callback, req);
        }

        if (connect(req->sock_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            req->state = API_STATE_SENDING;
            if (api_build_http_request(req) != 0) {
                if (g_api_reactor) {
                    reactor_remove_fd(g_api_reactor, req->sock_fd);
                }
                close(req->sock_fd);
                req->sock_fd = -1;
                return -1;
            }
            return 0;
        }

        if (errno == EINPROGRESS) {
            req->state = API_STATE_CONNECTING;
            return 0;
        }

        if (g_api_reactor) {
            reactor_remove_fd(g_api_reactor, req->sock_fd);
        }
        close(req->sock_fd);
        req->sock_fd = -1;
        return -1;
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", req->port);

    if (getaddrinfo(req->host, port_str, &hints, &req->addr_info) != 0) {
        LOG_ERROR("API: getaddrinfo failed for %s", req->host);
        return -1;
    }

    req->current_addr = req->addr_info;

    while (req->current_addr) {
        req->sock_fd = socket(req->current_addr->ai_family,
                               req->current_addr->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC,
                               req->current_addr->ai_protocol);
        if (req->sock_fd < 0) {
            req->current_addr = req->current_addr->ai_next;
            continue;
        }

        if (g_api_reactor) {
            reactor_add_fd(g_api_reactor, req->sock_fd,
                           REACTOR_EVENT_READ | REACTOR_EVENT_WRITE,
                           api_io_callback, req);
        }

        if (connect(req->sock_fd, req->current_addr->ai_addr,
                    req->current_addr->ai_addrlen) == 0) {
            req->state = API_STATE_SENDING;
            if (api_build_http_request(req) != 0) {
                if (g_api_reactor) {
                    reactor_remove_fd(g_api_reactor, req->sock_fd);
                }
                close(req->sock_fd);
                req->sock_fd = -1;
                return -1;
            }
            return 0;
        }

        if (errno == EINPROGRESS) {
            req->state = API_STATE_CONNECTING;
            return 0;
        }

        if (g_api_reactor) {
            reactor_remove_fd(g_api_reactor, req->sock_fd);
        }
        close(req->sock_fd);
        req->sock_fd = -1;
        req->current_addr = req->current_addr->ai_next;
    }

    freeaddrinfo(req->addr_info);
    req->addr_info = NULL;
    req->current_addr = NULL;

    LOG_ERROR("API: all connection attempts failed for %s:%d", req->host, req->port);
    return -1;
}
static int api_parse_header_line(const char *line, size_t len, char **name, char **value) {
    if (!line || !name || !value || len == 0) return -1;

    const char *colon = memchr(line, ':', len);
    if (!colon) return -1;

    size_t name_len = colon - line;
    if (name_len == 0) return -1;

    const char *val_start = colon + 1;
    while (val_start < line + len && (*val_start == ' ' || *val_start == '\t')) {
        val_start++;
    }

    size_t val_len = (line + len) - val_start;
    while (val_len > 0 && (val_start[val_len - 1] == ' ' ||
                           val_start[val_len - 1] == '\t' ||
                           val_start[val_len - 1] == '\r')) {
        val_len--;
    }

    *name = malloc(name_len + 1);
    if (!*name) return -1;
    memcpy(*name, line, name_len);
    (*name)[name_len] = '\0';

    *value = malloc(val_len + 1);
    if (!*value) {
        free(*name);
        *name = NULL;
        return -1;
    }
    memcpy(*value, val_start, val_len);
    (*value)[val_len] = '\0';

    return 0;
}

static int api_parse_status_line(const char *line, int *code) {
    if (!line || !code) return -1;

    if (strncmp(line, "HTTP/1.0", 8) != 0 &&
        strncmp(line, "HTTP/1.1", 8) != 0) {
        return -1;
    }

    const char *p = strchr(line, ' ');
    if (!p) return -1;
    p++;

    char *endptr;
    long c = strtol(p, &endptr, 10);
    if (endptr == p || c < 100 || c > 599) {
        return -1;
    }

    if (*endptr != ' ' && *endptr != '\r' && *endptr != '\0') {
        return -1;
    }

    *code = (int)c;
    return 0;
}

static int api_parse_headers(api_request_t *req) {
    char *header_end = strstr(req->recv_buf, "\r\n\r\n");
    if (!header_end) return 0;

    size_t header_len = header_end - req->recv_buf;

    if (header_len > API_MAX_HEADER_SIZE) {
        LOG_ERROR("API: headers too large (%zu bytes)", header_len);
        return -1;
    }

    req->headers_complete = 1;

    if (api_parse_status_line(req->recv_buf, &req->http_code) != 0) {
        LOG_ERROR("API: invalid status line");
        return -1;
    }

    req->content_length = -1;
    req->bytes_to_read = 0;
    req->chunked_encoding = 0;
    req->read_until_close = 0;
    int seen_cl = 0;
    int seen_te = 0;

    const char *line_start = req->recv_buf;
    while (line_start < header_end) {
        const char *line_end = memchr(line_start, '\n', header_end - line_start);
        if (!line_end) break;

        size_t line_len = line_end - line_start;
        if (line_len > 0 && line_start[line_len - 1] == '\r') {
            line_len--;
        }

        if (line_len > 0) {
            char *name = NULL;
            char *value = NULL;

            if (api_parse_header_line(line_start, line_len, &name, &value) == 0) {
                if (strcasecmp(name, "Content-Length") == 0) {
                    if (seen_cl) {
                        LOG_ERROR("API: duplicate Content-Length header");
                        free(name);
                        free(value);
                        return -1;
                    }
                    seen_cl = 1;

                    char *endptr;
                    long cl = strtol(value, &endptr, 10);

                    while (*endptr == ' ' || *endptr == '\t') {
                        endptr++;
                    }

                    if (*endptr != '\0') {
                        LOG_ERROR("API: malformed Content-Length: '%s'", value);
                        free(name);
                        free(value);
                        return -1;
                    }

                    if (cl < 0 || cl > API_MAX_RESPONSE_SIZE) {
                        LOG_ERROR("API: invalid Content-Length: %ld", cl);
                        free(name);
                        free(value);
                        return -1;
                    }
                    req->content_length = (int)cl;
                    req->bytes_to_read = (int)cl;
                } else if (strcasecmp(name, "Transfer-Encoding") == 0) {
                    if (seen_te) {
                        LOG_ERROR("API: duplicate Transfer-Encoding header");
                        free(name);
                        free(value);
                        return -1;
                    }
                    seen_te = 1;

                    if (strcasecmp(value, "chunked") == 0) {
                        req->chunked_encoding = 1;
                    }
                } else if (strcasecmp(name, "Connection") == 0) {
                    if (strcasecmp(value, "close") == 0) {
                        req->keepalive_disabled = 1;
                    }
                }

                free(name);
                free(value);
            }
        }

        line_start = line_end + 1;
    }

    if (seen_cl && seen_te) {
        LOG_ERROR("API: Content-Length and Transfer-Encoding conflict");
        return -1;
    }

    if (req->http_code == 204 || req->http_code == 304 ||
        strncmp(req->method, "HEAD", 4) == 0) {
        req->bytes_to_read = 0;
    }

    if (!req->chunked_encoding && req->content_length < 0 &&
        req->http_code >= 100 && req->http_code < 300 &&
        req->http_code != 204 && req->http_code != 304) {
        req->read_until_close = 1;
        LOG_DEBUG("API: read-until-close mode");
    }

    req->parse_state = HTTP_PARSE_BODY_CONTENT_LENGTH;
    if (req->chunked_encoding) {
        req->parse_state = HTTP_PARSE_BODY_CHUNKED;
        req->chunk_state = CHUNK_READ_SIZE;
        req->chunk_size = 0;
        req->chunk_offset = 0;
    } else if (req->read_until_close) {
        req->parse_state = HTTP_PARSE_BODY_CLOSE;
    }

    char *body_start = header_end + 4;
    if (body_start >= req->recv_buf + req->recv_offset) {
        req->raw_body_received = 0;
        req->body_received = 0;
        return 1;
    }

    req->chunk_parse_offset = body_start - req->recv_buf;
    req->raw_body_received = req->recv_offset - req->chunk_parse_offset;

    if (req->parse_state == HTTP_PARSE_BODY_CONTENT_LENGTH) {
        req->body_received = req->raw_body_received;
        req->decoded_body_len = req->raw_body_received;
    } else if (req->parse_state == HTTP_PARSE_BODY_CHUNKED) {
        req->body_received = 0;
        req->decoded_body_len = 0;
    } else {
        req->body_received = req->raw_body_received;
    }

    LOG_DEBUG("API: headers parsed, HTTP %d, CL=%d, TE=%s, read_until=%s",
              req->http_code, req->content_length,
              req->chunked_encoding ? "chunked" : "none",
              req->read_until_close ? "yes" : "no");

    return 1;
}
static int api_decode_chunked(api_request_t *req) {
    const char *buf = req->recv_buf + req->chunk_parse_offset;
    size_t avail = req->recv_offset - req->chunk_parse_offset;

    while (avail > 0 && req->chunk_state != CHUNK_DONE) {
        switch (req->chunk_state) {
            case CHUNK_READ_SIZE: {
                void *crlf_ptr = api_memmem(buf, avail, "\r\n", 2);
                if (!crlf_ptr) {
                    req->chunk_parse_offset = req->recv_offset - avail;
                    return 0;
                }

                size_t line_len = (const char *)crlf_ptr - buf;
                if (line_len == 0 || line_len > 32) {
                    LOG_ERROR("API: invalid chunk size line");
                    return -1;
                }

                char size_buf[64];
                memcpy(size_buf, buf, line_len);
                size_buf[line_len] = '\0';

                char *endptr;
                long size = strtol(size_buf, &endptr, 16);
                if (endptr == size_buf || size < 0) {
                    LOG_ERROR("API: invalid chunk size");
                    return -1;
                }

                if ((size_t)size > API_MAX_RESPONSE_SIZE) {
                    LOG_ERROR("API: chunk size too large: %ld", size);
                    return -1;
                }

                req->chunk_size = (size_t)size;
                req->chunk_offset = 0;

                size_t consumed = (const char *)crlf_ptr - buf + 2;
                req->chunk_parse_offset += consumed;
                avail -= consumed;
                buf += consumed;

                if (req->chunk_size == 0) {
                    req->chunk_state = CHUNK_READ_TRAILER;
                    LOG_DEBUG("API: chunked transfer complete, reading trailer");
                } else {
                    req->chunk_state = CHUNK_READ_DATA;
                }
                break;
            }

            case CHUNK_READ_DATA: {
                size_t to_read = req->chunk_size - req->chunk_offset;
                if (to_read > avail) {
                    to_read = avail;
                }

                if (to_read > 0) {
                    if (!req->decoded_body) {
                        req->decoded_body_size = API_CHUNK_READ_BUFFER;
                        req->decoded_body = malloc(req->decoded_body_size + 1);
                        if (!req->decoded_body) {
                            LOG_ERROR("API: malloc decoded_body failed");
                            return -1;
                        }
                        req->decoded_body_len = 0;
                    }

                    if (req->decoded_body_len + to_read > API_MAX_RESPONSE_SIZE) {
                        LOG_ERROR("API: decoded body exceeds max response size (%zu)",
                                  API_MAX_RESPONSE_SIZE);
                        return -1;
                    }

                    if (req->decoded_body_len + to_read + 1 > req->decoded_body_size) {
                        size_t new_size = req->decoded_body_size * 2;
                        if (new_size > API_MAX_RESPONSE_SIZE) {
                            new_size = API_MAX_RESPONSE_SIZE;
                        }
                        if (req->decoded_body_len + to_read + 1 > new_size) {
                            LOG_ERROR("API: decoded body would exceed max response size");
                            return -1;
                        }
                        char *new_buf = realloc(req->decoded_body, new_size + 1);
                        if (!new_buf) {
                            LOG_ERROR("API: realloc decoded_body failed");
                            return -1;
                        }
                        req->decoded_body = new_buf;
                        req->decoded_body_size = new_size;
                    }

                    memcpy(req->decoded_body + req->decoded_body_len, buf, to_read);
                    req->decoded_body_len += to_read;
                    req->decoded_body[req->decoded_body_len] = '\0';
                    req->chunk_offset += to_read;

                    req->chunk_parse_offset += to_read;
                    avail -= to_read;
                    buf += to_read;
                }

                if (req->chunk_offset >= req->chunk_size) {
                    req->chunk_state = CHUNK_READ_CRLF;
                    LOG_DEBUG("API: chunk data read, expecting CRLF");
                }
                break;
            }

            case CHUNK_READ_CRLF: {
                if (avail < 2) {
                    req->chunk_parse_offset = req->recv_offset - avail;
                    return 0;
                }

                if (buf[0] != '\r' || buf[1] != '\n') {
                    LOG_ERROR("API: missing CRLF after chunk data");
                    return -1;
                }

                req->chunk_parse_offset += 2;
                avail -= 2;
                buf += 2;

                req->chunk_state = CHUNK_READ_SIZE;
                req->chunk_size = 0;
                req->chunk_offset = 0;

                LOG_DEBUG("API: chunk CRLF received");
                break;
            }

            case CHUNK_READ_TRAILER: {
                if (avail >= 2 && memcmp(buf, "\r\n", 2) == 0) {
                    req->chunk_parse_offset += 2;
                    req->chunk_state = CHUNK_DONE;
                    req->parse_state = HTTP_PARSE_DONE;
                    req->state = API_STATE_DONE;
                    LOG_DEBUG("API: chunked transfer complete (empty trailer)");
                    return 1;
                }

                void *trailer_end = api_memmem(buf, avail, "\r\n\r\n", 4);
                if (!trailer_end) {
                    if (avail > 4096) {
                        LOG_ERROR("API: trailer headers too large");
                        return -1;
                    }
                    req->chunk_parse_offset = req->recv_offset - avail;
                    return 0;
                }

                size_t trailer_len = (const char *)trailer_end - buf + 4;
                req->chunk_parse_offset += trailer_len;
                req->chunk_state = CHUNK_DONE;
                req->parse_state = HTTP_PARSE_DONE;
                req->state = API_STATE_DONE;
                LOG_DEBUG("API: chunked transfer complete (non-empty trailer)");
                return 1;
            }

            case CHUNK_DONE:
                break;
        }
    }

    if (req->chunk_state == CHUNK_DONE) {
        req->parse_state = HTTP_PARSE_DONE;
        req->state = API_STATE_DONE;
        LOG_DEBUG("API: chunked decoding complete, %zu bytes decoded",
                  req->decoded_body_len);
        return 1;
    }

    return 0;
}

static const char *api_extract_body(api_request_t *req) {
    if (!req->recv_buf || req->recv_offset == 0) return NULL;

    if (req->chunked_encoding && req->decoded_body) {
        req->decoded_body[req->decoded_body_len] = '\0';
        return req->decoded_body;
    }

    char *body = strstr(req->recv_buf, "\r\n\r\n");
    if (!body) return NULL;

    if (body + 4 > req->recv_buf + req->recv_offset) return NULL;

    body += 4;
    if ((size_t)(body - req->recv_buf) >= req->recv_offset) return NULL;

    return body;
}
static void api_io_callback(reactor_t *r, int fd, uint32_t events, void *userdata) {
    (void)r;
    api_request_t *req = userdata;
    if (!req) return;

    if (events & (REACTOR_EVENT_ERROR | REACTOR_EVENT_HANGUP)) {
        LOG_ERROR("API: socket error on fd %d", fd);
        req->state = API_STATE_ERROR;
        req->keepalive_disabled = 1;
        api_process_requests(req->ctx);
        return;
    }

    switch (req->state) {
        case API_STATE_CONNECTING:
            if (events & REACTOR_EVENT_WRITE) {
                int error = 0;
                socklen_t len = sizeof(error);

                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) == 0 && error == 0) {
                    req->state = API_STATE_SENDING;
                    if (api_build_http_request(req) != 0) {
                        req->state = API_STATE_ERROR;
                        api_process_requests(req->ctx);
                        break;
                    }
                    LOG_DEBUG("API: connected to %s:%d", req->host, req->port);
                } else {
                    LOG_ERROR("API: connect error: %s", strerror(error));
                    req->state = API_STATE_ERROR;
                }
                api_process_requests(req->ctx);
            }
            break;

        case API_STATE_SENDING:
            if (events & REACTOR_EVENT_WRITE) {
                ssize_t sent = send(fd, req->send_buf + req->send_offset,
                                    req->send_len - req->send_offset, MSG_NOSIGNAL);
                if (sent > 0) {
                    req->send_offset += sent;
                    if (req->send_offset >= req->send_len) {
                        req->state = API_STATE_RECEIVING;
                        free(req->send_buf);
                        req->send_buf = NULL;
                        reactor_modify_fd(r, fd, REACTOR_EVENT_READ);
                        LOG_DEBUG("API: request sent, waiting for response");
                    }
                } else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    LOG_ERROR("API: send failed: %s", strerror(errno));
                    req->state = API_STATE_ERROR;
                    api_process_requests(req->ctx);
                }
            }
            break;

        case API_STATE_RECEIVING:
            if (events & REACTOR_EVENT_READ) {
                if (!req->recv_buf) {
                    req->recv_size = 4096;
                    req->recv_buf = malloc(req->recv_size);
                    if (!req->recv_buf) {
                        req->state = API_STATE_ERROR;
                        api_process_requests(req->ctx);
                        break;
                    }
                }

                if (req->recv_size >= API_MAX_RESPONSE_SIZE) {
                    LOG_ERROR("API: response too large");
                    req->state = API_STATE_ERROR;
                    req->keepalive_disabled = 1;
                    api_process_requests(req->ctx);
                    break;
                }

                if (!req->headers_complete &&
                    req->recv_offset > API_MAX_HEADER_SIZE) {
                    LOG_ERROR("API: headers exceed max size (%d bytes)", API_MAX_HEADER_SIZE);
                    req->state = API_STATE_ERROR;
                    req->keepalive_disabled = 1;
                    api_process_requests(req->ctx);
                    break;
                }

                if (req->recv_offset >= req->recv_size - 1) {
                    size_t new_size = req->recv_size * 2;
                    if (new_size > API_MAX_RESPONSE_SIZE) {
                        LOG_ERROR("API: response would exceed max size");
                        req->state = API_STATE_ERROR;
                        req->keepalive_disabled = 1;
                        api_process_requests(req->ctx);
                        break;
                    }
                    char *new_buf = realloc(req->recv_buf, new_size);
                    if (!new_buf) {
                        req->state = API_STATE_ERROR;
                        api_process_requests(req->ctx);
                        break;
                    }
                    req->recv_buf = new_buf;
                    req->recv_size = new_size;
                }

                ssize_t recvd = recv(fd, req->recv_buf + req->recv_offset,
                                     req->recv_size - req->recv_offset - 1, 0);
                if (recvd > 0) {
                    req->recv_offset += recvd;
                    req->recv_buf[req->recv_offset] = '\0';

                    if (!req->headers_complete) {
                        int ret = api_parse_headers(req);
                        if (ret < 0) {
                            req->state = API_STATE_ERROR;
                            req->keepalive_disabled = 1;
                            api_process_requests(req->ctx);
                            break;
                        }
                        if (ret > 0) {
                            if (req->parse_state == HTTP_PARSE_BODY_CONTENT_LENGTH) {
                                if (req->body_received >= (size_t)req->bytes_to_read) {
                                    req->state = API_STATE_DONE;
                                    LOG_DEBUG("API: response complete (Content-Length)");
                                    api_process_requests(req->ctx);
                                }
                            } else if (req->parse_state == HTTP_PARSE_BODY_CHUNKED) {
                                int chunk_ret = api_decode_chunked(req);
                                if (chunk_ret < 0) {
                                    req->state = API_STATE_ERROR;
                                    req->keepalive_disabled = 1;
                                    api_process_requests(req->ctx);
                                } else if (req->state == API_STATE_DONE) {
                                    api_process_requests(req->ctx);
                                }
                            }
                        }
                    } else {
                        req->raw_body_received += recvd;

                        if (req->parse_state == HTTP_PARSE_BODY_CONTENT_LENGTH) {
                            req->body_received += recvd;
                            if (req->body_received > (size_t)req->bytes_to_read) {
                                LOG_ERROR("API: response body exceeds Content-Length");
                                req->state = API_STATE_ERROR;
                                req->keepalive_disabled = 1;
                                api_process_requests(req->ctx);
                            } else if (req->body_received >= (size_t)req->bytes_to_read) {
                                req->state = API_STATE_DONE;
                                LOG_DEBUG("API: response complete (Content-Length)");
                                api_process_requests(req->ctx);
                            }
                        } else if (req->parse_state == HTTP_PARSE_BODY_CHUNKED) {
                            int chunk_ret = api_decode_chunked(req);
                            if (chunk_ret < 0) {
                                req->state = API_STATE_ERROR;
                                req->keepalive_disabled = 1;
                                api_process_requests(req->ctx);
                            } else if (req->state == API_STATE_DONE) {
                                api_process_requests(req->ctx);
                            }
                        } else if (req->parse_state == HTTP_PARSE_BODY_CLOSE) {
                            req->body_received += recvd;
                        }
                    }
                } else if (recvd == 0) {
                    if (req->parse_state == HTTP_PARSE_BODY_CLOSE ||
                        req->parse_state == HTTP_PARSE_BODY_CONTENT_LENGTH) {
                        req->state = API_STATE_DONE;
                        req->keepalive_disabled = 1;
                        LOG_DEBUG("API: peer closed connection, response complete");
                        api_process_requests(req->ctx);
                    } else if (req->parse_state == HTTP_PARSE_BODY_CHUNKED &&
                               req->chunk_state == CHUNK_DONE) {
                        req->state = API_STATE_DONE;
                        req->keepalive_disabled = 1;
                        LOG_DEBUG("API: chunked response complete");
                        api_process_requests(req->ctx);
                    } else if (req->parse_state == HTTP_PARSE_BODY_CHUNKED &&
                               req->chunk_state == CHUNK_READ_TRAILER) {
                        req->state = API_STATE_DONE;
                        req->keepalive_disabled = 1;
                        LOG_DEBUG("API: chunked trailer complete");
                        api_process_requests(req->ctx);
                    } else {
                        LOG_ERROR("API: unexpected connection close");
                        req->state = API_STATE_ERROR;
                        req->keepalive_disabled = 1;
                        api_process_requests(req->ctx);
                    }
                } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    LOG_ERROR("API: recv failed: %s", strerror(errno));
                    req->state = API_STATE_ERROR;
                    api_process_requests(req->ctx);
                }
            }
            break;

        default:
            break;
    }
}
static void api_process_requests(api_ctx_t *ctx) {
    api_request_t *prev = NULL;
    api_request_t *req = ctx->pending_requests;
    time_t now = time(NULL);

    while (req) {
        api_request_t *next = req->next;
        int should_remove = 0;

        if (now - req->start_time > ctx->timeout_sec) {
            LOG_ERROR("API: request timeout (%ds)", ctx->timeout_sec);
            req->state = API_STATE_ERROR;
        }

        if (req->state == API_STATE_DONE) {
            ctx->last_http_code = req->http_code;
            const char *body = api_extract_body(req);

            LOG_DEBUG("API: request success, HTTP %d", req->http_code);

            if (req->callback) {
                req->callback(req->http_code, body, req->userdata);
            }

            if (req->sock_fd >= 0 && !req->keepalive_disabled &&
                req->http_code >= 200 && req->http_code < 300 &&
                !req->read_until_close && !req->chunked_encoding) {
                if (ctx->keepalive_fd >= 0) {
                    if (g_api_reactor) {
                        reactor_remove_fd(g_api_reactor, ctx->keepalive_fd);
                    }
                    close(ctx->keepalive_fd);
                }
                ctx->keepalive_fd = req->sock_fd;
                ctx->keepalive_time = now;
                snprintf(ctx->keepalive_host, sizeof(ctx->keepalive_host), "%s", req->host);
                ctx->keepalive_port = req->port;
                req->sock_fd = -1;
                LOG_DEBUG("API: saved keep-alive connection (fd=%d)", ctx->keepalive_fd);
            }

            should_remove = 1;
        } else if (req->state == API_STATE_ERROR) {
            ctx->last_http_code = 0;
            snprintf(ctx->last_error, sizeof(ctx->last_error), "Request failed");
            LOG_DEBUG("API: request failed");
            if (req->callback) {
                req->callback(0, NULL, req->userdata);
            }

            if (ctx->keepalive_fd >= 0) {
                if (g_api_reactor) {
                    reactor_remove_fd(g_api_reactor, ctx->keepalive_fd);
                }
                close(ctx->keepalive_fd);
                ctx->keepalive_fd = -1;
                ctx->keepalive_host[0] = '\0';
                ctx->keepalive_port = 0;
                LOG_DEBUG("API: closed keep-alive connection due to error");
            }

            should_remove = 1;
        }

        if (should_remove) {
            if (prev) {
                prev->next = next;
            } else {
                ctx->pending_requests = next;
            }
            if (ctx->pending_count > 0) {
                ctx->pending_count--;
            } else {
                LOG_WARN("API: pending_count underflow detected");
            }
            api_request_cleanup(req);
        } else {
            prev = req;
        }

        req = next;
    }

    if (ctx->keepalive_fd >= 0 && now - ctx->keepalive_time > 60) {
        LOG_DEBUG("API: keep-alive connection expired");
        if (g_api_reactor) {
            reactor_remove_fd(g_api_reactor, ctx->keepalive_fd);
        }
        close(ctx->keepalive_fd);
        ctx->keepalive_fd = -1;
        ctx->keepalive_host[0] = '\0';
        ctx->keepalive_port = 0;
    }
}

int api_start_with_reactor(api_ctx_t *ctx, reactor_t *r) {
    if (!ctx || !r) return -1;
    g_api_reactor = r;
    LOG_DEBUG("API: Reactor registered");
    return 0;
}

int api_get_fds(api_ctx_t *ctx, int *fds, int max_fds) {
    int count = 0;
    api_request_t *req = ctx->pending_requests;

    while (req && count < max_fds) {
        if (req->sock_fd >= 0) {
            fds[count++] = req->sock_fd;
        }
        req = req->next;
    }

    return count;
}

int api_handle_event(api_ctx_t *ctx, int fd, int events) {
    (void)ctx;
    (void)fd;
    (void)events;
    return 0;
}

int api_process(api_ctx_t *ctx) {
    api_process_requests(ctx);
    return 0;
}

static int api_request_async(api_ctx_t *ctx, const char *method, const char *path,
                              const char *body, api_callback_t callback, void *userdata) {
    if (!ctx || !method || !path) return -1;

    if (ctx->pending_count >= API_MAX_PENDING_REQUESTS) {
        LOG_ERROR("API: too many pending requests (%d)", ctx->pending_count);
        return -1;
    }

    if (strchr(path, '\r') || strchr(path, '\n')) {
        LOG_ERROR("API: invalid path contains CR/LF");
        return -1;
    }

    api_request_t *req = calloc(1, sizeof(api_request_t));
    if (!req) return -1;

    req->ctx = ctx;
    req->state = API_STATE_IDLE;
    req->sock_fd = -1;
    req->content_length = -1;
    req->keepalive_disabled = 0;
    req->read_until_close = 0;
    req->chunked_encoding = 0;
    req->chunk_state = CHUNK_READ_SIZE;
    req->chunk_size = 0;
    req->chunk_offset = 0;
    req->chunk_parse_offset = 0;
    req->decoded_body = NULL;
    req->decoded_body_size = 0;
    req->decoded_body_len = 0;
    req->raw_body_received = 0;
    req->parse_state = HTTP_PARSE_HEADERS;
    snprintf(req->method, sizeof(req->method), "%s", method);
    snprintf(req->path, sizeof(req->path), "%s", path);
    req->body = body ? strdup(body) : NULL;
    req->callback = callback;
    req->userdata = userdata;
    req->start_time = time(NULL);

    if (api_parse_url(ctx->base_url, req->host, &req->port) != 0) {
        free(req->body);
        free(req);
        return -1;
    }

    req->next = ctx->pending_requests;
    ctx->pending_requests = req;
    ctx->pending_count++;

    LOG_DEBUG("API: async request queued: %s %s", method, path);

    if (api_socket_connect(req) != 0) {
        req->state = API_STATE_ERROR;
        api_process_requests(ctx);
    }

    return 0;
}

int api_get_mode_async(api_ctx_t *ctx, api_callback_t callback, void *userdata) {
    return api_request_async(ctx, "GET", "/configs", NULL, callback, userdata);
}

int api_set_mode_async(api_ctx_t *ctx, const char *mode, api_callback_t callback, void *userdata) {
    char json_body[256];
    snprintf(json_body, sizeof(json_body), "{\"mode\":\"%s\"}", mode);
    return api_request_async(ctx, "PATCH", "/configs", json_body, callback, userdata);
}

int api_check_health_async(api_ctx_t *ctx, api_callback_t callback, void *userdata) {
    return api_request_async(ctx, "GET", "/health", NULL, callback, userdata);
}

int api_get_proxies_async(api_ctx_t *ctx, api_callback_t callback, void *userdata) {
    return api_request_async(ctx, "GET", "/proxies", NULL, callback, userdata);
}

int api_request_raw_async(api_ctx_t *ctx, const char *method, const char *url,
                          const char *body, api_callback_t callback, void *userdata) {
    if (!ctx || !method || !url) return -1;

    if (ctx->pending_count >= API_MAX_PENDING_REQUESTS) {
        LOG_ERROR("API: too many pending requests (%d)", ctx->pending_count);
        return -1;
    }

    api_request_t *req = calloc(1, sizeof(api_request_t));
    if (!req) return -1;

    req->ctx = ctx;
    req->state = API_STATE_IDLE;
    req->sock_fd = -1;
    req->content_length = -1;
    req->keepalive_disabled = 1;
    req->read_until_close = 0;
    req->chunked_encoding = 0;
    req->chunk_state = CHUNK_READ_SIZE;
    req->chunk_size = 0;
    req->chunk_offset = 0;
    req->chunk_parse_offset = 0;
    req->decoded_body = NULL;
    req->decoded_body_size = 0;
    req->decoded_body_len = 0;
    req->raw_body_received = 0;
    req->parse_state = HTTP_PARSE_HEADERS;
    snprintf(req->method, sizeof(req->method), "%s", method);
    req->body = body ? strdup(body) : NULL;
    req->callback = callback;
    req->userdata = userdata;
    req->start_time = time(NULL);

    if (api_parse_full_url(url, req->host, &req->port, req->path, sizeof(req->path)) != 0) {
        free(req->body);
        free(req);
        return -1;
    }

    req->next = ctx->pending_requests;
    ctx->pending_requests = req;
    ctx->pending_count++;

    LOG_DEBUG("API: raw async request queued: %s %s", method, url);

    if (api_socket_connect(req) != 0) {
        req->state = API_STATE_ERROR;
        api_process_requests(ctx);
    }

    return 0;
}

int api_get_sync(const char *url, char *response, size_t response_size) {
    if (!url || !response || response_size == 0) return -1;

    if (strncmp(url, "https://", 8) == 0) {
        LOG_ERROR("API sync: HTTPS not supported");
        return -1;
    }

    int sock_fd = -1;
    int result = -1;
    char *recv_buf = NULL;
    size_t recv_size = 4096;
    size_t recv_offset = 0;
    int headers_complete = 0;
    int content_length = -1;
    int chunked = 0;
    int http_code = 0;
    size_t body_received = 0;
    size_t header_offset = 0;
    struct addrinfo *ai = NULL;

    char host[64];
    char path[256];
    int port;
    const char *start;

    if (strncmp(url, "http://", 7) == 0) {
        start = url + 7;
        port = 80;
    } else {
        start = url;
        port = 80;
    }

    const char *slash = strchr(start, '/');
    const char *colon = strchr(start, ':');

    if (colon && (!slash || colon < slash)) {
        size_t len = colon - start;
        safe_str_copy(host, sizeof(host), start, len);
        char *endptr;
        long p = strtol(colon + 1, &endptr, 10);
        if (endptr != colon + 1 && p >= 1 && p <= 65535) {
            port = (int)p;
        } else {
            port = 80;
        }
        if (slash) {
            safe_str_copy(path, sizeof(path), slash, strlen(slash));
        } else {
            safe_str_copy(path, sizeof(path), "/", 1);
        }
    } else if (slash) {
        size_t len = slash - start;
        safe_str_copy(host, sizeof(host), start, len);
        safe_str_copy(path, sizeof(path), slash, strlen(slash));
    } else {
        safe_str_copy(host, sizeof(host), start, strlen(start));
        safe_str_copy(path, sizeof(path), "/", 1);
    }

    if (port == 443) {
        LOG_ERROR("API sync: HTTPS not supported");
        return -1;
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(host, port_str, &hints, &ai) != 0) {
        LOG_ERROR("API sync: getaddrinfo failed for %s", host);
        return -1;
    }

    sock_fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (sock_fd < 0) {
        LOG_ERROR("API sync: socket failed: %s", strerror(errno));
        goto cleanup;
    }

    struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
    setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(sock_fd, ai->ai_addr, ai->ai_addrlen) < 0) {
        LOG_ERROR("API sync: connect failed: %s", strerror(errno));
        goto cleanup;
    }

    freeaddrinfo(ai);
    ai = NULL;

    char headers[2048];
    int header_len = snprintf(headers, sizeof(headers),
             "GET %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "User-Agent: ATPd/1.0\r\n"
             "Accept: */*\r\n"
             "Connection: close\r\n"
             "\r\n", path, host);

    if (header_len < 0 || header_len >= (int)sizeof(headers)) {
        LOG_ERROR("API sync: headers too long");
        goto cleanup;
    }

    ssize_t sent = send(sock_fd, headers, header_len, 0);
    if (sent != header_len) {
        LOG_ERROR("API sync: send failed: %s", strerror(errno));
        goto cleanup;
    }

    recv_buf = malloc(recv_size);
    if (!recv_buf) {
        LOG_ERROR("API sync: malloc failed");
        goto cleanup;
    }

    while (1) {
        if (recv_offset >= recv_size - 1) {
            size_t new_size = recv_size * 2;
            if (new_size > API_MAX_RESPONSE_SIZE) {
                LOG_ERROR("API sync: response too large");
                goto cleanup;
            }
            char *new_buf = realloc(recv_buf, new_size);
            if (!new_buf) goto cleanup;
            recv_buf = new_buf;
            recv_size = new_size;
        }

        ssize_t recvd = recv(sock_fd, recv_buf + recv_offset, recv_size - recv_offset - 1, 0);
        if (recvd > 0) {
            recv_offset += recvd;
            recv_buf[recv_offset] = '\0';

            if (!headers_complete) {
                char *header_end = strstr(recv_buf, "\r\n\r\n");
                if (header_end) {
                    header_offset = header_end - recv_buf + 4;
                    headers_complete = 1;

                    if (header_offset > API_MAX_HEADER_SIZE) {
                        LOG_ERROR("API sync: headers too large");
                        goto cleanup;
                    }

                    if (api_parse_status_line(recv_buf, &http_code) != 0) {
                        LOG_ERROR("API sync: invalid status line");
                        goto cleanup;
                    }

                    const char *line_start = recv_buf;
                    while (line_start < header_end) {
                        const char *line_end = memchr(line_start, '\n', header_end - line_start);
                        if (!line_end) break;

                        size_t line_len = line_end - line_start;
                        if (line_len > 0 && line_start[line_len - 1] == '\r') {
                            line_len--;
                        }

                        if (line_len > 0) {
                            char *name = NULL;
                            char *value = NULL;

                            if (api_parse_header_line(line_start, line_len, &name, &value) == 0) {
                                if (strcasecmp(name, "Content-Length") == 0) {
                                    char *endptr;
                                    long cl = strtol(value, &endptr, 10);
                                    if (cl >= 0 && cl <= API_MAX_RESPONSE_SIZE) {
                                        content_length = (int)cl;
                                    }
                                } else if (strcasecmp(name, "Transfer-Encoding") == 0) {
                                    if (strcasecmp(value, "chunked") == 0) {
                                        chunked = 1;
                                    }
                                }
                                free(name);
                                free(value);
                            }
                        }
                        line_start = line_end + 1;
                    }

                    if (chunked) {
                        LOG_ERROR("API sync: chunked encoding not supported in sync API");
                        goto cleanup;
                    }

                    if (http_code < 200 || http_code >= 300) {
                        LOG_ERROR("API sync: HTTP %d error", http_code);
                        goto cleanup;
                    }

                    if (content_length >= 0) {
                        body_received = recv_offset - header_offset;
                        if (body_received >= (size_t)content_length) {
                            break;
                        }
                    } else {
                        body_received = recv_offset - header_offset;
                    }
                } else {
                    if (recv_offset > API_MAX_HEADER_SIZE) {
                        LOG_ERROR("API sync: headers exceed max size");
                        goto cleanup;
                    }
                    continue;
                }
            } else {
                body_received += recvd;
                if (content_length >= 0 && body_received >= (size_t)content_length) {
                    break;
                }
            }
        } else if (recvd == 0) {
            if (headers_complete) {
                break;
            } else {
                LOG_ERROR("API sync: connection closed before headers");
                goto cleanup;
            }
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            LOG_ERROR("API sync: recv failed: %s", strerror(errno));
            goto cleanup;
        }
    }

    if (headers_complete && header_offset > 0 && recv_offset > header_offset) {
        size_t body_len = recv_offset - header_offset;
        if (body_len >= response_size) body_len = response_size - 1;
        memcpy(response, recv_buf + header_offset, body_len);
        response[body_len] = '\0';
        result = 0;
    }

cleanup:
    if (ai) freeaddrinfo(ai);
    if (sock_fd >= 0) close(sock_fd);
    free(recv_buf);
    return result;
}

int api_get_mode(api_ctx_t *ctx, char *mode, size_t size) {
    return api_get_mode_sync(ctx, mode, size);
}

int api_get_mode_sync(api_ctx_t *ctx, char *mode, size_t size) {
    if (!ctx || !mode || size == 0) return -1;

    char url[256];
    snprintf(url, sizeof(url), "%s/configs", ctx->base_url);

    char response[8192];
    if (api_get_sync(url, response, sizeof(response)) != 0) return -1;

    yyjson_doc *doc = yyjson_read(response, strlen(response), 0);
    if (!doc) return -1;

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *mode_item = yyjson_obj_get(root, "mode");
    if (mode_item && yyjson_is_str(mode_item)) {
        snprintf(mode, size, "%s", yyjson_get_str(mode_item));
        yyjson_doc_free(doc);
        return 0;
    }

    yyjson_doc_free(doc);
    return -1;
}

const char *api_mode_to_string(api_mode_t mode) {
    switch (mode) {
        case API_MODE_RULE:       return "Rule";
        case API_MODE_GLOBAL:     return "Global";
        case API_MODE_DIRECT:     return "Direct";
        case API_MODE_GOOGLE_VPN: return "Google VPN";
        default:                  return "Rule";
    }
}

api_mode_t api_string_to_mode(const char *str) {
    if (strcmp(str, "Global") == 0) return API_MODE_GLOBAL;
    if (strcmp(str, "Direct") == 0) return API_MODE_DIRECT;
    if (strcmp(str, "Google VPN") == 0) return API_MODE_GOOGLE_VPN;
    return API_MODE_RULE;
}