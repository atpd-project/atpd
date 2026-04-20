/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2025 ATP Project
 *
 * Clash API client - Pure async epoll-driven state machine
 * Zero blocking, zero libcurl, zero legacy
 */

#ifndef ATP_API_H
#define ATP_API_H

#include "atp.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <time.h>

typedef enum {
    API_STATE_IDLE = 0,
    API_STATE_CONNECTING,
    API_STATE_SENDING,
    API_STATE_RECEIVING,
    API_STATE_DONE,
    API_STATE_ERROR
} api_state_t;

struct api_request;

typedef void (*api_callback_t)(int http_code, const char *response, void *userdata);

typedef struct api_request {
    struct api_request *next;
    struct api_ctx *ctx;
    api_state_t state;
    int sock_fd;
    char method[8];
    char path[256];
    char *body;
    char host[64];
    int port;
    struct addrinfo *addr_info;
    struct addrinfo *current_addr;
    
    char *send_buf;
    size_t send_len;
    size_t send_offset;
    
    char *recv_buf;
    size_t recv_size;
    size_t recv_offset;
    
    int http_code;
    int content_length;
    int headers_complete;
    size_t bytes_to_read;
    size_t body_received;
    
    api_callback_t callback;
    void *userdata;
    
    time_t start_time;
} api_request_t;

typedef struct api_ctx {
    char base_url[128];
    char secret[64];
    int timeout_sec;
    int last_http_code;
    char last_error[256];
    api_request_t *pending_requests;
} api_ctx_t;

typedef enum {
    API_MODE_RULE = 0,
    API_MODE_GLOBAL = 1,
    API_MODE_DIRECT = 2,
    API_MODE_GOOGLE_VPN = 3
} api_mode_t;

extern api_ctx_t g_api_ctx;

int api_init(api_ctx_t *ctx, atp_config_t *cfg);
void api_cleanup(api_ctx_t *ctx);

int api_get_mode_async(api_ctx_t *ctx, api_callback_t callback, void *userdata);
int api_set_mode_async(api_ctx_t *ctx, const char *mode, api_callback_t callback, void *userdata);
int api_check_health_async(api_ctx_t *ctx, api_callback_t callback, void *userdata);

int api_get_fds(api_ctx_t *ctx, int *fds, int max_fds);
int api_handle_event(api_ctx_t *ctx, int fd, int events);
int api_process(api_ctx_t *ctx);

int api_get_mode(api_ctx_t *ctx, char *mode, size_t size);

const char *api_mode_to_string(api_mode_t mode);
api_mode_t api_string_to_mode(const char *str);

#endif /* ATP_API_H */
