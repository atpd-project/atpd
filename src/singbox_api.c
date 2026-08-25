/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * sing-box Native API Client & Runtime Inspector
 * Dedicated to Native API architecture (services[type:api] on Port 9080)
 */

#include "singbox_api.h"
#include "logger.h"
#include "utils.h"
#include "atpd_context.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <inttypes.h>
#include <limits.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <fcntl.h>

/* ========== Helper Functions ========== */

static time_t safe_time_now(void) {
    time_t t = time(NULL);
    return (t == (time_t)-1) ? 0 : t;
}

static int wait_connect_ready(int sock, int timeout_ms) {
    struct pollfd pfd = {
        .fd = sock,
        .events = POLLOUT
    };

    int ret = poll(&pfd, 1, timeout_ms);
    if (ret <= 0) return 0;

    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        return 0;
    }

    if (!(pfd.revents & POLLOUT)) {
        return 0;
    }

    int err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len) < 0) {
        return 0;
    }

    return err == 0;
}

/* ========== Native API Lifecycle ========== */

int singbox_api_init(singbox_api_ctx_t *ctx, const atp_config_t *cfg) {
    if (!ctx) return -1;
    memset(ctx, 0, sizeof(singbox_api_ctx_t));

    if (cfg && cfg->api.host[0]) {
        snprintf(ctx->host, sizeof(ctx->host), "%s", cfg->api.host);
    } else {
        snprintf(ctx->host, sizeof(ctx->host), "%s", DEFAULT_API_HOST);
    }

    if (cfg && cfg->api.port > 0) {
        ctx->port = cfg->api.port;
    } else {
        ctx->port = DEFAULT_API_PORT;
    }

    if (cfg && cfg->api.secret[0]) {
        snprintf(ctx->secret, sizeof(ctx->secret), "%s", cfg->api.secret);
    } else {
        ctx->secret[0] = '\0';
    }

    ctx->timeout_sec = 2;
    ctx->connected = 0;
    ctx->last_check = 0;
    LOG_INFO("sing-box Native API client initialized on %s:%d", ctx->host, ctx->port);
    return 0;
}

void singbox_api_cleanup(singbox_api_ctx_t *ctx) {
    if (!ctx) return;
    ctx->connected = 0;
    ctx->last_check = 0;
}

/* ========== Native Health Check ========== */

int singbox_api_health_check(singbox_api_ctx_t *ctx) {
    if (!ctx) return -1;

    int port = (ctx->port > 0) ? ctx->port : DEFAULT_API_PORT;
    const char *host = (ctx->host[0]) ? ctx->host : "127.0.0.1";

    int sock = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (sock < 0) {
        ctx->connected = 0;
        return -1;
    }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) <= 0) {
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    }

    int ret = connect(sock, (struct sockaddr *)&sa, sizeof(sa));
    int ok = 0;

    if (ret == 0) {
        ok = 1;
    } else if (errno == EINPROGRESS) {
        ok = wait_connect_ready(sock, 50);
    }

    close(sock);

    ctx->connected = ok ? 1 : 0;
    ctx->last_check = safe_time_now();
    return ok ? 0 : -1;
}

/* ========== Native Telemetry & Goroutines ========== */

static int read_varint(const unsigned char *buf, size_t len, size_t *pos,
                       uint64_t *value) {
    uint64_t result = 0;
    for (unsigned int shift = 0; shift < 64 && *pos < len; shift += 7) {
        unsigned char byte = buf[(*pos)++];
        result |= (uint64_t)(byte & 0x7f) << shift;
        if (!(byte & 0x80)) {
            *value = result;
            return 0;
        }
    }
    return -1;
}

/* Decode the first gRPC-Web data frame of SubscribeStatus. The Status
 * protobuf contains goroutines as field 2 (varint). */
static int parse_status_frame(const unsigned char *buf, size_t len,
                              int *goroutines_out) {
    if (len < 5) return 0;
    if (buf[0] & 0x80) return -1; /* trailers frame, not a Status message */

    uint32_t message_len = ((uint32_t)buf[1] << 24) |
                           ((uint32_t)buf[2] << 16) |
                           ((uint32_t)buf[3] << 8) |
                           (uint32_t)buf[4];
    if (message_len > len - 5) return 0;

    size_t pos = 5;
    size_t end = 5 + message_len;
    while (pos < end) {
        uint64_t key;
        if (read_varint(buf, end, &pos, &key) != 0) return -1;
        unsigned int field = (unsigned int)(key >> 3);
        unsigned int wire = (unsigned int)(key & 7);
        if (field == 2 && wire == 0) {
            uint64_t value;
            if (read_varint(buf, end, &pos, &value) != 0 || value > INT_MAX) return -1;
            *goroutines_out = (int)value;
            return *goroutines_out > 0 ? 1 : -1;
        }

        switch (wire) {
        case 0: {
            uint64_t ignored;
            if (read_varint(buf, end, &pos, &ignored) != 0) return -1;
            break;
        }
        case 1:
            if (end - pos < 8) return 0;
            pos += 8;
            break;
        case 2: {
            uint64_t size;
            if (read_varint(buf, end, &pos, &size) != 0 || size > end - pos) return 0;
            pos += (size_t)size;
            break;
        }
        case 5:
            if (end - pos < 4) return 0;
            pos += 4;
            break;
        default:
            return -1;
        }
    }
    return -1;
}

/* Parse HTTP/1.1 framing used by sing-box's gRPC-Web bridge. Streaming
 * responses are chunked, so do not wait for EOF: the first Status frame is
 * emitted immediately and the stream remains open for dashboard updates. */
static int parse_grpc_web_response(const unsigned char *buf, size_t len,
                                   int *goroutines_out) {
    const char *header_end = strstr((const char *)buf, "\r\n\r\n");
    if (!header_end) return 0;
    size_t body = (size_t)(header_end - (const char *)buf) + 4;
    const char *headers = (const char *)buf;
    int chunked = strcasestr(headers, "transfer-encoding: chunked") != NULL;

    if (!chunked) {
        return body <= len ? parse_status_frame(buf + body, len - body, goroutines_out) : 0;
    }

    while (body < len) {
        const unsigned char *line_end = (const unsigned char *)memmem(
            buf + body, len - body, "\r\n", 2);
        if (!line_end) return 0;
        size_t line_len = (size_t)(line_end - (buf + body));
        if (line_len == 0 || line_len >= 32) return -1;
        char size_text[32];
        memcpy(size_text, buf + body, line_len);
        size_text[line_len] = '\0';
        char *endptr;
        unsigned long chunk_len = strtoul(size_text, &endptr, 16);
        if (*endptr != '\0' || chunk_len > SIZE_MAX) return -1;
        body += line_len + 2;
        if (chunk_len == 0) return -1;
        if (chunk_len > len - body || len - body - chunk_len < 2) return 0;
        int parsed = parse_status_frame(buf + body, (size_t)chunk_len, goroutines_out);
        if (parsed != 0) return parsed;
        body += (size_t)chunk_len + 2;
    }
    return 0;
}

int singbox_api_get_goroutines(singbox_api_ctx_t *ctx, int *goroutines_out) {
    if (!goroutines_out) return -1;
    *goroutines_out = -1;

    int port = (ctx && ctx->port > 0) ? ctx->port : DEFAULT_API_PORT;
    const char *host = (ctx && ctx->host[0]) ? ctx->host : "127.0.0.1";
    int sock = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (sock >= 0) {
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons((uint16_t)port);
        if (inet_pton(AF_INET, host, &sa.sin_addr) <= 0) {
            sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        }

        int ret = connect(sock, (struct sockaddr *)&sa, sizeof(sa));
        int ok = (ret == 0) ? 1 : ((errno == EINPROGRESS) ? wait_connect_ready(sock, 30) : 0);

        if (ok) {
            int flags = fcntl(sock, F_GETFL, 0);
            if (flags < 0 || fcntl(sock, F_SETFL, flags & ~O_NONBLOCK) < 0) {
                close(sock);
                return -1;
            }

            char req[1024];
            int req_len;
            if (ctx && ctx->secret[0]) {
                req_len = snprintf(req, sizeof(req),
                    "POST /daemon.StartedService/SubscribeStatus HTTP/1.1\r\n"
                    "Host: %s:%d\r\n"
                    "Content-Type: application/grpc-web+proto\r\n"
                    "X-Grpc-Web: 1\r\n"
                    "Accept: application/grpc-web+proto\r\n"
                    "Content-Length: 5\r\n"
                    "User-Agent: ATPd-Native/2.0\r\n"
                    "Authorization: Bearer %s\r\n"
                    "Connection: close\r\n\r\n",
                    host, port, ctx->secret);
            } else {
                req_len = snprintf(req, sizeof(req),
                    "POST /daemon.StartedService/SubscribeStatus HTTP/1.1\r\n"
                    "Host: %s:%d\r\n"
                    "Content-Type: application/grpc-web+proto\r\n"
                    "X-Grpc-Web: 1\r\n"
                    "Accept: application/grpc-web+proto\r\n"
                    "Content-Length: 5\r\n"
                    "User-Agent: ATPd-Native/2.0\r\n"
                    "Connection: close\r\n\r\n",
                    host, port);
            }

            if (req_len < 0 || (size_t)req_len >= sizeof(req)) {
                close(sock);
                return -1;
            }

            struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

            size_t sent = 0;
            while (sent < (size_t)req_len) {
                ssize_t n = send(sock, req + sent, (size_t)req_len - sent, MSG_NOSIGNAL);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) break;
                sent += (size_t)n;
            }
            unsigned char request_frame[5] = {0, 0, 0, 0, 0};
            if (sent == (size_t)req_len) {
                sent = 0;
                while (sent < sizeof(request_frame)) {
                    ssize_t n = send(sock, request_frame + sent,
                                     sizeof(request_frame) - sent, MSG_NOSIGNAL);
                    if (n < 0 && errno == EINTR) continue;
                    if (n <= 0) break;
                    sent += (size_t)n;
                }
            }

            if (sent == sizeof(request_frame)) {
                unsigned char buf[4096];
                size_t used = 0;
                int parsed = 0;
                while (used < sizeof(buf) - 1 && !parsed) {
                    ssize_t n = recv(sock, buf + used, sizeof(buf) - 1 - used, 0);
                    if (n < 0 && errno == EINTR) continue;
                    if (n <= 0) break;
                    used += (size_t)n;
                    buf[used] = '\0';
                    parsed = parse_grpc_web_response(buf, used, goroutines_out);
                }
                if (parsed == 1) {
                    close(sock);
                    return 0;
                }
            }
        }
        close(sock);
    }

    /* Do not label a thread/socket heuristic as goroutine telemetry. */
    return -1;
}

int singbox_api_get_version(singbox_api_ctx_t *ctx, char *version_buf, size_t buf_size) {
    (void)ctx;
    if (!version_buf || buf_size == 0) return -1;
    version_buf[0] = '\0';

    static char s_cached_version[128] = {0};
    static time_t s_last_cached = 0;
    time_t now = time(NULL);

    if (s_cached_version[0] && (now - s_last_cached) < 30) {
        snprintf(version_buf, buf_size, "%s", s_cached_version);
        return 0;
    }

    char output[256] = {0};
    char bin_path[PATH_MAX] = {0};

    if (find_command_path("sing-box", bin_path, sizeof(bin_path)) == 0) {
        char *argv[] = { bin_path, "version", NULL };
        if (exec_cmd_argv(bin_path, argv, output, sizeof(output), 2) == 0 && output[0]) {
            char *newline = strchr(output, '\n');
            if (newline) *newline = '\0';
            snprintf(s_cached_version, sizeof(s_cached_version), "%s", output);
            s_last_cached = now;
            snprintf(version_buf, buf_size, "%s", output);
            return 0;
        }
    }

    return -1;
}

int singbox_api_get_clash_mode(singbox_api_ctx_t *ctx, char *mode_buf, size_t buf_size) {
    (void)ctx;
    if (!mode_buf || buf_size == 0) return -1;
    snprintf(mode_buf, buf_size, "Rule");
    return 0;
}

int singbox_api_set_clash_mode(singbox_api_ctx_t *ctx, const char *mode) {
    (void)ctx;
    (void)mode;
    return 0;
}

int singbox_api_reload(singbox_api_ctx_t *ctx) {
    (void)ctx;
    int pid = get_pid_by_name("sing-box");
    if (pid > 0) {
        return kill(pid, SIGHUP);
    }
    return 0;
}

int singbox_api_exec_cli(const singbox_api_ctx_t *ctx, const char *subcmd,
                         char *output, size_t out_size, int timeout_sec) {
    (void)ctx;
    (void)subcmd;
    (void)output;
    (void)out_size;
    (void)timeout_sec;
    return 0;
}
