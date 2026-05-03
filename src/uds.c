/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * Unix Domain Socket - Runtime CLI command interface
 */

#include "reactor.h"
#include "logger.h"
#include "atpd_context.h"
#include "session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>

#define UDS_BUFFER_SIZE 256
#define UDS_RESPONSE_SIZE 1024

static int g_uds_fd = -1;

/* ========== Response Helpers ========== */

static void get_stage_name(char *buf, size_t size) {
    const char *stage = "unknown";
    if (g_atpd_ctx.vpn_state == VPN_STATE_READY) stage = "READY";
    else if (g_atpd_ctx.vpn_state == VPN_STATE_PREDICTING) stage = "PREDICTING";
    else if (g_atpd_ctx.vpn_state == VPN_STATE_TEARDOWN) stage = "TEARDOWN";
    else stage = "IDLE";
    snprintf(buf, size, "%s", stage);
}

static uint64_t get_total_spliced(void) {
    return g_atpd_ctx.splice_bytes_total;
}

static const char* get_vpn_sync_status(void) {
    return vpn_state_string(g_atpd_ctx.vpn_state);
}

/* ========== UDS Client Callback ========== */

static void uds_client_cb(reactor_t *r, int fd, uint32_t events, void *userdata) {
    (void)r;
    (void)userdata;

    if (events & (REACTOR_EVENT_ERROR | REACTOR_EVENT_HANGUP)) {
        LOG_DEBUG("UDS: client disconnected (fd=%d)", fd);
        reactor_remove_fd(r, fd);
        close(fd);
        return;
    }

    if (!(events & REACTOR_EVENT_READ)) return;

    char buf[UDS_BUFFER_SIZE];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) {
        LOG_DEBUG("UDS: client closed connection (fd=%d)", fd);
        reactor_remove_fd(r, fd);
        close(fd);
        return;
    }

    buf[n] = '\0';

    /* Remove trailing newline */
    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';

    char response[UDS_RESPONSE_SIZE];
    int response_len = 0;

    if (strncmp(buf, "status", 6) == 0) {
        char stage[32];
        get_stage_name(stage, sizeof(stage));
        response_len = snprintf(response, sizeof(response),
            "STAGE: %s\n"
            "PUMP: %llu bytes\n"
            "XFRM_STATE: %s\n"
            "VPN_IFACE: %s\n"
            "TRANSITIONS: %llu\n",
            stage,
            (unsigned long long)get_total_spliced(),
            get_vpn_sync_status(),
            g_atpd_ctx.vpn_iface[0] ? g_atpd_ctx.vpn_iface : "none",
            (unsigned long long)g_atpd_ctx.vpn_transitions);
    }
    else if (strncmp(buf, "stop", 4) == 0) {
        atpd_session_emergency_drain_all();
        response_len = snprintf(response, sizeof(response), "OK: Kill-Switch activated\n");
    }
    else if (strncmp(buf, "ping", 4) == 0) {
        response_len = snprintf(response, sizeof(response), "PONG\n");
    }
    else {
        response_len = snprintf(response, sizeof(response),
            "ERROR: unknown command '%s'\n"
            "Available: status, stop, ping\n", buf);
    }

    if (response_len > 0 && response_len < UDS_RESPONSE_SIZE) {
        write(fd, response, (size_t)response_len);
    }
}

/* ========== UDS Accept Callback ========== */

static void uds_accept_cb(reactor_t *r, int listen_fd, uint32_t events, void *userdata) {
    (void)events;
    (void)userdata;

    int client_fd = accept4(listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (client_fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            LOG_ERROR("UDS: accept failed: %s", strerror(errno));
        }
        return;
    }

    reactor_add_fd(r, client_fd, REACTOR_EVENT_READ, uds_client_cb, NULL);
    LOG_DEBUG("UDS: client connected (fd=%d)", client_fd);
}

/* ========== Init ========== */

int uds_init(reactor_t *r, const char *path) {
    struct sockaddr_un addr;

    g_uds_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (g_uds_fd < 0) {
        LOG_ERROR("UDS: socket failed: %s", strerror(errno));
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    unlink(path);

    if (bind(g_uds_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("UDS: bind to %s failed: %s", path, strerror(errno));
        close(g_uds_fd);
        return -1;
    }

    chmod(path, 0666);

    if (listen(g_uds_fd, 8) < 0) {
        LOG_ERROR("UDS: listen failed: %s", strerror(errno));
        close(g_uds_fd);
        unlink(path);
        return -1;
    }

    reactor_add_fd(r, g_uds_fd, REACTOR_EVENT_READ, uds_accept_cb, NULL);
    LOG_INFO("UDS: listening on %s", path);

    return 0;
}

void uds_cleanup(void) {
    if (g_uds_fd >= 0) {
        close(g_uds_fd);
        g_uds_fd = -1;
    }
}

int uds_get_fd(void) {
    return g_uds_fd;
}
