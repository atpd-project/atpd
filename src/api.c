#include "atpd_global.h"
/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2025 ATP Project
 *
 * Clash API client - Reactor-driven async state machine
 * Zero blocking, zero libcurl, zero legacy
 * HTTP/1.1 Keep-Alive support
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

/* ========== Reactor Integration ========== */

static reactor_t *g_api_reactor = NULL;

/* ========== Forward Declarations ========== */

static void api_parse_url(const char *base_url, char *host, int *port);
static void api_parse_full_url(const char *url, char *host, int *port, char *path, size_t path_size);
static int api_build_http_request(api_request_t *req);
static void api_request_cleanup(api_request_t *req);
static int api_socket_connect(api_request_t *req);
static int api_parse_headers(api_request_t *req);
static const char *api_extract_body(api_request_t *req);
static void api_io_callback(reactor_t *r, int fd, uint32_t events, void *userdata);
static void api_process_requests(api_ctx_t *ctx);

/* ========== Global Context ========== */


/* ========== URL Parsing ========== */

static void api_parse_url(const char *base_url, char *host, int *port) {
    const char *start;
    
    if (strncmp(base_url, "http://", 7) == 0) {
        start = base_url + 7;
        *port = 80;
    } else {
        start = base_url;
        *port = 80;
    }
    
    const char *colon = strchr(start, ':');
    const char *slash = strchr(start, '/');
    
    if (colon && (!slash || colon < slash)) {
        size_t len = colon - start;
        strncpy(host, start, len);
        host[len] = '\0';
        *port = atoi(colon + 1);
    } else if (slash) {
        size_t len = slash - start;
        strncpy(host, start, len);
        host[len] = '\0';
    } else {
        strcpy(host, start);
    }
}

static void api_parse_full_url(const char *url, char *host, int *port, char *path, size_t path_size) {
    const char *start;

    if (strncmp(url, "https://", 8) == 0) {
        LOG_ERROR("API: HTTPS not supported for raw requests");
        start = url + 8;
        *port = 443;
    } else if (strncmp(url, "http://", 7) == 0) {
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
        strncpy(host, start, len);
        host[len] = '\0';
        *port = atoi(colon + 1);
        if (slash) {
            strncpy(path, slash, path_size - 1);
        } else {
            strcpy(path, "/");
        }
    } else if (slash) {
        size_t len = slash - start;
        strncpy(host, start, len);
        host[len] = '\0';
        strncpy(path, slash, path_size - 1);
    } else {
        strcpy(host, start);
        strcpy(path, "/");
    }
}

/* ========== Core API Functions ========== */

int api_init(api_ctx_t *ctx, atp_config_t *cfg) {
    memset(ctx, 0, sizeof(api_ctx_t));
    
    snprintf(ctx->base_url, sizeof(ctx->base_url), "http://%s:%d", 
             cfg->api_host, cfg->api_port);
    
    if (cfg->clash_secret[0] != '\0') {
        strncpy(ctx->secret, cfg->clash_secret, sizeof(ctx->secret) - 1);
    }
    
    ctx->timeout_sec = 2;
    ctx->pending_requests = NULL;
    ctx->keepalive_fd = -1;
    ctx->keepalive_time = 0;
    
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
    
    if (ctx->keepalive_fd >= 0) {
        if (g_api_reactor) {
            reactor_remove_fd(g_api_reactor, ctx->keepalive_fd);
        }
        close(ctx->keepalive_fd);
        ctx->keepalive_fd = -1;
    }
    
    g_api_reactor = NULL;
}

/* ========== Request Building ========== */

static int api_build_http_request(api_request_t *req) {
    char headers[2048];
    int header_len;
    
    header_len = snprintf(headers, sizeof(headers),
             "%s %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "User-Agent: ATPd/1.0\r\n"
             "Accept: application/json\r\n"
             "Connection: keep-alive\r\n",
             req->method, req->path, req->host);
    
    if (req->ctx->secret[0]) {
        header_len += snprintf(headers + header_len, sizeof(headers) - header_len,
                               "Authorization: Bearer %s\r\n", req->ctx->secret);
    }
    
    if (req->body) {
        header_len += snprintf(headers + header_len, sizeof(headers) - header_len,
                               "Content-Type: application/json\r\n"
                               "Content-Length: %zu\r\n", strlen(req->body));
    }
    
    header_len += snprintf(headers + header_len, sizeof(headers) - header_len, "\r\n");
    
    size_t body_len = req->body ? strlen(req->body) : 0;
    req->send_buf = malloc(header_len + body_len + 1);
    if (!req->send_buf) {
        LOG_ERROR("API: malloc send_buf failed");
        return -1;
    }
    
    memcpy(req->send_buf, headers, header_len);
    if (req->body) {
        memcpy(req->send_buf + header_len, req->body, body_len);
    }
    
    req->send_len = header_len + body_len;
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
    
    req->body = NULL;
    req->send_buf = NULL;
    req->recv_buf = NULL;
    
    free(req);
}

/* ========== Socket Connection ========== */

static int api_socket_connect(api_request_t *req) {
    api_ctx_t *ctx = req->ctx;
    
    if (ctx->keepalive_fd >= 0) {
        int error = 0;
        socklen_t len = sizeof(error);
        if (getsockopt(ctx->keepalive_fd, SOL_SOCKET, SO_ERROR, &error, &len) == 0 && error == 0) {
            req->sock_fd = ctx->keepalive_fd;
            ctx->keepalive_fd = -1;
            if (g_api_reactor) {
                reactor_remove_fd(g_api_reactor, req->sock_fd);
            }
            
            req->state = API_STATE_SENDING;
            api_build_http_request(req);
            
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
            api_build_http_request(req);
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
            api_build_http_request(req);
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
            api_build_http_request(req);
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

/* ========== HTTP Parsing ========== */

static int api_parse_headers(api_request_t *req) {
    char *header_end = strstr(req->recv_buf, "\r\n\r\n");
    if (!header_end) return 0;
    
    req->headers_complete = 1;
    
    char *status_line = req->recv_buf;
    if (strncmp(status_line, "HTTP/", 5) == 0) {
        status_line = strchr(status_line, ' ');
    }
    if (status_line) {
        req->http_code = atoi(status_line + 1);
    }
    
    char *cl = strcasestr(req->recv_buf, "Content-Length:");
    if (cl) {
        cl = strchr(cl, ':');
        if (cl) {
            cl++;
            while (*cl == ' ') cl++;
            req->content_length = strtol(cl, NULL, 10);
            req->bytes_to_read = req->content_length;
        } else {
            req->content_length = -1;
            req->bytes_to_read = 0;
        }
    } else {
        req->content_length = -1;
        req->bytes_to_read = 0;
    }
    
    char *conn = strcasestr(req->recv_buf, "Connection:");
    if (conn) {
        conn = strchr(conn, ':') + 1;
        while (*conn == ' ') conn++;
        if (strncasecmp(conn, "close", 5) == 0) {
            req->keepalive_disabled = 1;
        }
    }
    
    if (req->http_code == 204 || req->http_code == 304 ||
        strncmp(req->method, "HEAD", 4) == 0) {
        req->bytes_to_read = 0;
    }
    
    char *body_start = header_end + 4;
    req->body_received = req->recv_offset - (body_start - req->recv_buf);
    
    LOG_DEBUG("API: headers parsed, HTTP %d, Content-Length: %d, keepalive: %s",
              req->http_code, req->content_length,
              req->keepalive_disabled ? "disabled" : "enabled");
    
    return 1;
}

static const char *api_extract_body(api_request_t *req) {
    if (!req->recv_buf || req->recv_offset == 0) return NULL;
    char *body = strstr(req->recv_buf, "\r\n\r\n");
    if (!body) return NULL;
    body += 4;
    if ((size_t)(body - req->recv_buf) >= req->recv_offset) return NULL;
    return body;
}

/* ========== Reactor I/O Callback ========== */

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
                    api_build_http_request(req);
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
                }
                
                if (req->recv_offset >= req->recv_size - 1) {
                    req->recv_size *= 2;
                    char *new_buf = realloc(req->recv_buf, req->recv_size);
                    if (!new_buf) {
                        req->state = API_STATE_ERROR;
                        api_process_requests(req->ctx);
                        break;
                    }
                    req->recv_buf = new_buf;
                }
                
                ssize_t recvd = recv(fd, req->recv_buf + req->recv_offset,
                                     req->recv_size - req->recv_offset - 1, 0);
                if (recvd > 0) {
                    req->recv_offset += recvd;
                    req->recv_buf[req->recv_offset] = '\0';
                    
                    if (!req->headers_complete) {
                        if (api_parse_headers(req)) {
                            char *body_start = strstr(req->recv_buf, "\r\n\r\n") + 4;
                            req->body_received = req->recv_offset - (body_start - req->recv_buf);
                            
                            if (req->body_received >= req->bytes_to_read) {
                                req->state = API_STATE_DONE;
                                LOG_DEBUG("API: response complete in header parsing");
                                api_process_requests(req->ctx);
                            }
                        }
                    } else {
                        req->body_received += recvd;
                        
                        if (req->body_received > req->bytes_to_read) {
                            LOG_ERROR("API: response body exceeds Content-Length");
                            req->state = API_STATE_ERROR;
                            req->keepalive_disabled = 1;
                            api_process_requests(req->ctx);
                        } else if (req->body_received >= req->bytes_to_read) {
                            req->state = API_STATE_DONE;
                            LOG_DEBUG("API: response complete, total %zu bytes", req->body_received);
                            api_process_requests(req->ctx);
                        }
                    }
                } else if (recvd == 0) {
                    req->state = API_STATE_DONE;
                    req->keepalive_disabled = 1;
                    LOG_DEBUG("API: peer closed connection");
                    api_process_requests(req->ctx);
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

/* ========== Request Processing ========== */

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
                req->http_code != 404 && req->http_code != 500 && req->http_code != 502) {
                if (ctx->keepalive_fd >= 0) {
                    if (g_api_reactor) {
                        reactor_remove_fd(g_api_reactor, ctx->keepalive_fd);
                    }
                    close(ctx->keepalive_fd);
                }
                ctx->keepalive_fd = req->sock_fd;
                ctx->keepalive_time = now;
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
    }
}

/* ========== Reactor Registration ========== */

int api_start_with_reactor(api_ctx_t *ctx, reactor_t *r) {
    if (!ctx || !r) return -1;
    g_api_reactor = r;
    LOG_DEBUG("API: Reactor registered");
    return 0;
}

/* ========== Legacy Compatibility Functions ========== */

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

/* ========== Async API ========== */

static int api_request_async(api_ctx_t *ctx, const char *method, const char *path,
                              const char *body, api_callback_t callback, void *userdata) {
    api_request_t *req = calloc(1, sizeof(api_request_t));
    if (!req) return -1;
    
    req->ctx = ctx;
    req->state = API_STATE_IDLE;
    req->sock_fd = -1;
    req->content_length = -1;
    req->keepalive_disabled = 0;
    strncpy(req->method, method, sizeof(req->method) - 1);
    strncpy(req->path, path, sizeof(req->path) - 1);
    req->body = body ? strdup(body) : NULL;
    req->callback = callback;
    req->userdata = userdata;
    req->start_time = time(NULL);
    
    api_parse_url(ctx->base_url, req->host, &req->port);
    
    req->next = ctx->pending_requests;
    ctx->pending_requests = req;
    
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
    api_request_t *req = calloc(1, sizeof(api_request_t));
    if (!req) return -1;

    req->ctx = ctx;
    req->state = API_STATE_IDLE;
    req->sock_fd = -1;
    req->content_length = -1;
    req->keepalive_disabled = 1;
    strncpy(req->method, method, sizeof(req->method) - 1);
    req->body = body ? strdup(body) : NULL;
    req->callback = callback;
    req->userdata = userdata;
    req->start_time = time(NULL);

    api_parse_full_url(url, req->host, &req->port, req->path, sizeof(req->path));

    req->next = ctx->pending_requests;
    ctx->pending_requests = req;

    LOG_DEBUG("API: raw async request queued: %s %s", method, url);

    if (api_socket_connect(req) != 0) {
        req->state = API_STATE_ERROR;
        api_process_requests(ctx);
    }

    return 0;
}

/* ========== Sync API ========== */

int api_get_sync(const char *url, char *response, size_t response_size) {
    if (!url || !response || response_size == 0) return -1;

    int sock_fd = -1;
    int result = -1;
    char *recv_buf = NULL;
    size_t recv_size = 4096;
    size_t recv_offset = 0;

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
        strncpy(host, start, len);
        host[len] = '\0';
        port = atoi(colon + 1);
        if (slash) {
            strncpy(path, slash, sizeof(path) - 1);
            path[sizeof(path) - 1] = '\0';
        } else {
            strcpy(path, "/");
        }
    } else if (slash) {
        size_t len = slash - start;
        strncpy(host, start, len);
        host[len] = '\0';
        strncpy(path, slash, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
    } else {
        strcpy(host, start);
        strcpy(path, "/");
    }

    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        LOG_ERROR("API sync: socket failed: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (strcmp(host, "localhost") == 0 || strcmp(host, "127.0.0.1") == 0) {
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    } else if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        struct hostent *he = gethostbyname(host);
        if (!he) {
            LOG_ERROR("API sync: DNS failed for %s", host);
            goto cleanup;
        }
        memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    }

    struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
    setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("API sync: connect failed: %s", strerror(errno));
        goto cleanup;
    }

    char headers[2048];
    int header_len = snprintf(headers, sizeof(headers),
             "GET %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "User-Agent: ATPd/1.0\r\n"
             "Accept: */*\r\n"
             "Connection: close\r\n"
             "\r\n", path, host);

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
            recv_size *= 2;
            char *new_buf = realloc(recv_buf, recv_size);
            if (!new_buf) goto cleanup;
            recv_buf = new_buf;
        }

        ssize_t recvd = recv(sock_fd, recv_buf + recv_offset, recv_size - recv_offset - 1, 0);
        if (recvd > 0) {
            recv_offset += recvd;
            recv_buf[recv_offset] = '\0';
        } else if (recvd == 0) {
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            LOG_ERROR("API sync: recv failed: %s", strerror(errno));
            goto cleanup;
        }
    }

    char *body = strstr(recv_buf, "\r\n\r\n");
    if (body) {
        body += 4;
        size_t body_len = recv_offset - (body - recv_buf);
        if (body_len >= response_size) body_len = response_size - 1;
        memcpy(response, body, body_len);
        response[body_len] = '\0';
        result = 0;
    }

cleanup:
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
        strncpy(mode, yyjson_get_str(mode_item), size - 1);
        mode[size - 1] = '\0';
        yyjson_doc_free(doc);
        return 0;
    }

    yyjson_doc_free(doc);
    return -1;
}

/* ========== Mode Conversion ========== */

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
