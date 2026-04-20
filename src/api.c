/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2025 ATP Project
 *
 * Clash API client - Native async socket state machine
 */

#include "api.h"
#include "logger.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <cjson/cJSON.h>

static void api_parse_url(const char *base_url, char *host, int *port);
static int api_build_http_request(api_request_t *req);
static void api_request_cleanup(api_request_t *req);
static int api_socket_connect(api_request_t *req);
static int api_extract_http_code(api_request_t *req);
static const char *api_extract_body(api_request_t *req);

static void api_parse_url(const char *base_url, char *host, int *port) {
    const char *start;
    const char *end;
    
    if (strncmp(base_url, "http://", 7) == 0) {
        start = base_url + 7;
        *port = 80;
    } else {
        start = base_url;
        *port = 80;
    }
    
    end = strchr(start, ':');
    if (end) {
        size_t len = end - start;
        strncpy(host, start, len);
        host[len] = '\0';
        *port = atoi(end + 1);
    } else {
        end = strchr(start, '/');
        if (end) {
            size_t len = end - start;
            strncpy(host, start, len);
            host[len] = '\0';
        } else {
            strcpy(host, start);
        }
    }
}

int api_init(api_ctx_t *ctx, atp_config_t *cfg) {
    memset(ctx, 0, sizeof(api_ctx_t));
    
    snprintf(ctx->base_url, sizeof(ctx->base_url), "http://%s:%d", 
             cfg->api_host, cfg->api_port);
    
    if (cfg->clash_secret[0] != '\0') {
        strncpy(ctx->secret, cfg->clash_secret, sizeof(ctx->secret) - 1);
    }
    
    ctx->retry_count = 3;
    ctx->retry_delay_ms = 500;
    ctx->min_interval_ms = 100;
    ctx->timeout_sec = 2;
    ctx->pending_requests = NULL;
    
    LOG_INFO("API initialized (native async): %s", ctx->base_url);
    return 0;
}

void api_cleanup(api_ctx_t *ctx) {
    api_request_t *req = ctx->pending_requests;
    while (req) {
        api_request_t *next = req->next;
        api_request_cleanup(req);
        req = next;
    }
    ctx->pending_requests = NULL;
}

static int api_build_http_request(api_request_t *req) {
    char headers[2048];
    int header_len;
    
    header_len = snprintf(headers, sizeof(headers),
             "%s %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "User-Agent: ATPd/1.0\r\n"
             "Accept: application/json\r\n"
             "Connection: close\r\n",
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
    if (req->sock_fd >= 0) {
        close(req->sock_fd);
    }
    if (req->addr_info) {
        freeaddrinfo(req->addr_info);
    }
    free(req->body);
    free(req->send_buf);
    free(req->recv_buf);
    free(req);
}

static int api_socket_connect(api_request_t *req) {
    struct addrinfo hints;
    
    if (!req->addr_info) {
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
    }
    
    while (req->current_addr) {
        req->sock_fd = socket(req->current_addr->ai_family,
                               req->current_addr->ai_socktype | SOCK_NONBLOCK,
                               req->current_addr->ai_protocol);
        if (req->sock_fd < 0) {
            req->current_addr = req->current_addr->ai_next;
            continue;
        }
        
        int flags = fcntl(req->sock_fd, F_GETFL, 0);
        fcntl(req->sock_fd, F_SETFL, flags | O_NONBLOCK);
        
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
        
        close(req->sock_fd);
        req->sock_fd = -1;
        req->current_addr = req->current_addr->ai_next;
    }
    
    LOG_ERROR("API: all connection attempts failed for %s:%d", req->host, req->port);
    return -1;
}

int api_request_async(api_ctx_t *ctx, const char *method, const char *path,
                      const char *body, api_callback_t callback, void *userdata) {
    api_request_t *req = calloc(1, sizeof(api_request_t));
    if (!req) return -1;
    
    req->ctx = ctx;
    req->state = API_STATE_IDLE;
    req->sock_fd = -1;
    strncpy(req->method, method, sizeof(req->method) - 1);
    strncpy(req->path, path, sizeof(req->path) - 1);
    req->body = body ? strdup(body) : NULL;
    req->callback = callback;
    req->userdata = userdata;
    req->start_time = time(NULL);
    
    api_parse_url(ctx->base_url, req->host, &req->port);
    
    req->next = ctx->pending_requests;
    ctx->pending_requests = req;
    
    req->state = API_STATE_RESOLVING;
    
    LOG_DEBUG("API async: %s %s", method, path);
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
    api_request_t *req = ctx->pending_requests;
    
    while (req) {
        if (req->sock_fd == fd) {
            break;
        }
        req = req->next;
    }
    
    if (!req) {
        return -1;
    }
    
    switch (req->state) {
        case API_STATE_CONNECTING:
            if (events & EPOLLOUT) {
                int error = 0;
                socklen_t len = sizeof(error);
                
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) == 0 && error == 0) {
                    req->state = API_STATE_SENDING;
                    api_build_http_request(req);
                    LOG_DEBUG("API connected to %s:%d", req->host, req->port);
                } else {
                    LOG_ERROR("API connect failed: %s", strerror(error));
                    req->state = API_STATE_ERROR;
                }
            }
            break;
            
        case API_STATE_SENDING:
            if (events & EPOLLOUT) {
                ssize_t sent = send(fd, req->send_buf + req->send_offset,
                                    req->send_len - req->send_offset, 0);
                if (sent > 0) {
                    req->send_offset += sent;
                    if (req->send_offset >= req->send_len) {
                        req->state = API_STATE_RECEIVING;
                        free(req->send_buf);
                        req->send_buf = NULL;
                        LOG_DEBUG("API request sent, waiting for response");
                    }
                } else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    LOG_ERROR("API send failed: %s", strerror(errno));
                    req->state = API_STATE_ERROR;
                }
            }
            break;
            
        case API_STATE_RECEIVING:
            if (events & EPOLLIN) {
                if (!req->recv_buf) {
                    req->recv_size = 4096;
                    req->recv_buf = malloc(req->recv_size);
                }
                
                if (req->recv_offset >= req->recv_size - 1) {
                    req->recv_size *= 2;
                    char *new_buf = realloc(req->recv_buf, req->recv_size);
                    if (!new_buf) {
                        req->state = API_STATE_ERROR;
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
                        char *header_end = strstr(req->recv_buf, "\r\n\r\n");
                        if (header_end) {
                            req->headers_complete = 1;
                            req->http_code = api_extract_http_code(req);
                        }
                    }
                } else if (recvd == 0) {
                    req->state = API_STATE_DONE;
                    LOG_DEBUG("API response received, HTTP %d", req->http_code);
                } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    LOG_ERROR("API recv failed: %s", strerror(errno));
                    req->state = API_STATE_ERROR;
                }
            }
            break;
            
        default:
            break;
    }
    
    return 0;
}

int api_process(api_ctx_t *ctx) {
    api_request_t *prev = NULL;
    api_request_t *req = ctx->pending_requests;
    int processed = 0;
    time_t now = time(NULL);
    
    while (req) {
        api_request_t *next = req->next;
        int should_remove = 0;
        
        if (now - req->start_time > ctx->timeout_sec) {
            req->state = API_STATE_ERROR;
            strcpy(ctx->last_error, "Request timeout");
        }
        
        switch (req->state) {
            case API_STATE_RESOLVING:
                if (api_socket_connect(req) == 0) {
                    req->state = API_STATE_CONNECTING;
                } else {
                    req->state = API_STATE_ERROR;
                }
                processed++;
                break;
                
            case API_STATE_CONNECTING:
            case API_STATE_SENDING:
            case API_STATE_RECEIVING:
                processed++;
                break;
                
            case API_STATE_DONE:
                ctx->last_http_code = req->http_code;
                if (req->callback) {
                    const char *body = api_extract_body(req);
                    req->callback(req->http_code, body, req->userdata);
                }
                should_remove = 1;
                break;
                
            case API_STATE_ERROR:
                ctx->last_http_code = 0;
                if (req->callback) {
                    req->callback(0, NULL, req->userdata);
                }
                should_remove = 1;
                break;
                
            default:
                break;
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
    
    return processed;
}

static int api_extract_http_code(api_request_t *req) {
    if (!req->recv_buf) return 0;
    
    char *status_line = strchr(req->recv_buf, ' ');
    if (status_line) {
        return atoi(status_line + 1);
    }
    return 0;
}

static const char *api_extract_body(api_request_t *req) {
    if (!req->recv_buf) return NULL;
    
    char *body = strstr(req->recv_buf, "\r\n\r\n");
    if (body) {
        return body + 4;
    }
    return NULL;
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
