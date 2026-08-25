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

int singbox_api_get_goroutines(singbox_api_ctx_t *ctx, int *goroutines_out) {
    if (!goroutines_out) return -1;
    *goroutines_out = -1;

    static int s_cached_goroutines = 0;
    static time_t s_last_cached = 0;
    time_t now = time(NULL);

    if (s_cached_goroutines > 0 && (now - s_last_cached) < 2) {
        *goroutines_out = s_cached_goroutines;
        return 0;
    }

    int port = (ctx && ctx->port > 0) ? ctx->port : DEFAULT_API_PORT;
    const char *host = (ctx && ctx->host[0]) ? ctx->host : "127.0.0.1";

    /* Query the Go pprof endpoint exposed by sing-box. The connect phase is
     * non-blocking, but the request/response phase must drain until EOF: a
     * pprof response commonly exceeds one TCP segment. */
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

            char req[768];
            int req_len;
            if (ctx && ctx->secret[0]) {
                req_len = snprintf(req, sizeof(req),
                    "GET /debug/pprof/goroutine?debug=1 HTTP/1.1\r\n"
                    "Host: %s:%d\r\n"
                    "User-Agent: ATPd-Native/2.0\r\n"
                    "Authorization: Bearer %s\r\n"
                    "Connection: close\r\n\r\n",
                    host, port, ctx->secret);
            } else {
                req_len = snprintf(req, sizeof(req),
                    "GET /debug/pprof/goroutine?debug=1 HTTP/1.1\r\n"
                    "Host: %s:%d\r\n"
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

            if (sent == (size_t)req_len) {
                char buf[16384];
                size_t used = 0;
                while (used < sizeof(buf) - 1) {
                    ssize_t n = recv(sock, buf + used, sizeof(buf) - 1 - used, 0);
                    if (n < 0 && errno == EINTR) continue;
                    if (n <= 0) break;
                    used += (size_t)n;
                }
                buf[used] = '\0';
                char *p = strstr(buf, "goroutine profile: total ");
                if (p) {
                    int count = 0;
                    if (sscanf(p + strlen("goroutine profile: total "), "%d", &count) == 1 && count > 0) {
                        close(sock);
                        s_cached_goroutines = count;
                        s_last_cached = now;
                        *goroutines_out = count;
                        return 0;
                    }
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
