/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * Unix Domain Socket - Runtime CLI command interface
 * Production Ready
 */

#include "reactor.h"
#include "uds.h"
#include "logger.h"
#include "atpd_context.h"
#include "status.h"
#include "ui.h"
#include "session.h"
#include "atpd_error.h"
#include "version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <inttypes.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <pwd.h>

#define UDS_BUFFER_SIZE 4096
#define UDS_RESPONSE_SIZE 8192
#define UDS_BACKLOG 16
#define UDS_PATH_MAX 108

static int g_uds_fd = -1;
static char g_uds_path[UDS_PATH_MAX] = {0};
static reactor_t *g_uds_reactor = NULL;
static int g_uds_stop_requested = 0;
static uds_dependencies_t g_uds_dependencies;

/* ========== Safe Response Builder ========== */

static size_t append_response(char *buf, size_t size, size_t offset, const char *fmt, ...) {
    va_list args;
    int ret;

    if (offset >= size) {
        return size;
    }

    va_start(args, fmt);
    ret = vsnprintf(buf + offset, size - offset, fmt, args);
    va_end(args);

    if (ret < 0) {
        return offset;
    }

    if ((size_t)ret >= size - offset) {
        return size;
    }

    return offset + (size_t)ret;
}

/* ========== Response Helpers ========== */

static int send_response_all(int fd, const char *resp, size_t len) {
    if (!resp || len == 0) return 0;

    while (len > 0) {
        ssize_t n = send(fd, resp, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                LOG_DEBUG("UDS: send would block, partial response");
                return -1;
            }
            if (errno == EPIPE || errno == ECONNRESET) {
                LOG_DEBUG("UDS: client disconnected during send");
                return 0;
            }
            LOG_DEBUG("UDS: send failed: %s", strerror(errno));
            return 0;
        }
        if (n == 0) {
            return 0;
        }
        resp += n;
        len -= (size_t)n;
    }
    return 1;
}

static void send_string_all(int fd, const char *str) {
    if (str) {
        send_response_all(fd, str, strlen(str));
    }
}

static int check_client_uid(int fd) {
    struct ucred cred;
    socklen_t len = sizeof(cred);

    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0) {
        LOG_DEBUG("UDS: getsockopt SO_PEERCRED failed: %s", strerror(errno));
        return 0;
    }

    if (cred.uid != 0 && cred.uid != getuid()) {
        LOG_WARN("UDS: rejected connection from uid=%d", cred.uid);
        return 0;
    }

    LOG_DEBUG("UDS: accepted connection from uid=%d, pid=%d", cred.uid, cred.pid);
    return 1;
}

static void handle_status(int fd) {
    char *buf = NULL;
    size_t size = 0;
    FILE *mem = open_memstream(&buf, &size);
    if (mem) {
        ui_set_output_file(mem);
        status_show(g_uds_dependencies.config,
                    g_uds_dependencies.service,
                    g_uds_dependencies.api);
        ui_set_output_file(NULL);
        fclose(mem);

        if (buf && size > 0) {
            send_response_all(fd, buf, size);
        }
        free(buf);
    } else {
        send_string_all(fd, "ERROR: failed to generate status\n");
    }
}

static void handle_stop(int fd) {
    send_string_all(fd, "Stopping ATPd...\n");

    LOG_INFO("UDS: received stop command, initiating shutdown");
    atpd_runtime_state_transition(ATPD_RUNTIME_STATE_STOPPING);
    atpd_session_emergency_drain_all();

    g_uds_stop_requested = 1;
    if (g_uds_dependencies.shutdown_requested) {
        *g_uds_dependencies.shutdown_requested = 1;
    }
    if (g_uds_reactor) {
        reactor_stop(g_uds_reactor);
    }
}

static void handle_ping(int fd) {
    send_string_all(fd, "pong\n");
}

static void handle_sessions(int fd) {
    char response[UDS_RESPONSE_SIZE];
    size_t off = 0;

    off = append_response(response, sizeof(response), off,
                          "Active Sessions: %d\n",
                          atpd_session_active_count());

    if (off >= sizeof(response)) {
        send_string_all(fd, "ERROR: response too large\n");
        return;
    }

    send_response_all(fd, response, off);
}

static void handle_version(int fd) {
    char response[UDS_RESPONSE_SIZE];
    size_t off = 0;

    off = append_response(response, sizeof(response), off,
                          "ATPd Version: %s\n", atp_get_full_version());
    off = append_response(response, sizeof(response), off,
                          "Git Commit: %s\n", atp_get_commit());

    if (off >= sizeof(response)) {
        send_string_all(fd, "ERROR: response too large\n");
        return;
    }

    send_response_all(fd, response, off);
}

static void handle_stats(int fd) {
    char response[UDS_RESPONSE_SIZE];
    size_t off = 0;
    const reactor_stats_t *stats = reactor_get_stats(g_uds_reactor);

    off = append_response(response, sizeof(response), off,
                          "=== Statistics ===\n");
    off = append_response(response, sizeof(response), off,
                          "Events Processed: %" PRIu64 "\n",
                          stats ? stats->events_processed : 0);
    off = append_response(response, sizeof(response), off,
                          "Timers Fired: %" PRIu64 "\n",
                          stats ? stats->timers_fired : 0);
    off = append_response(response, sizeof(response), off,
                          "Signals Received: %" PRIu64 "\n",
                          stats ? stats->signals_received : 0);
    off = append_response(response, sizeof(response), off,
                          "Errors: %" PRIu64 "\n",
                          atpd_error_total());

    if (off >= sizeof(response)) {
        send_string_all(fd, "ERROR: response too large\n");
        return;
    }

    send_response_all(fd, response, off);
}

static void handle_help(int fd) {
    const char *help =
        "Available commands:\n"
        "  status    - Show runtime status\n"
        "  stop      - Shutdown ATPd\n"
        "  ping      - Check if ATPd is alive\n"
        "  sessions  - Show active sessions\n"
        "  version   - Show version information\n"
        "  stats     - Show statistics\n"
        "  help      - Show this help\n";
    send_string_all(fd, help);
}

/* ========== Command Processing ========== */

static void process_command(int fd, const char *cmd, size_t cmd_len) {
    char buf[UDS_BUFFER_SIZE];

    if (cmd_len == 0) {
        return;
    }

    if (cmd_len >= sizeof(buf) - 1) {
        send_string_all(fd, "ERROR: command too long\n");
        return;
    }

    memcpy(buf, cmd, cmd_len);
    buf[cmd_len] = '\0';

    char *newline = strchr(buf, '\n');
    if (newline) *newline = '\0';

    char *cr = strchr(buf, '\r');
    if (cr) *cr = '\0';

    char *end = buf + strlen(buf) - 1;
    while (end >= buf && (*end == ' ' || *end == '\t')) {
        *end = '\0';
        end--;
    }

    if (buf[0] == '\0') {
        return;
    }

    if (strcmp(buf, "status") == 0) {
        handle_status(fd);
    } else if (strcmp(buf, "stop") == 0) {
        handle_stop(fd);
    } else if (strcmp(buf, "ping") == 0) {
        handle_ping(fd);
    } else if (strcmp(buf, "sessions") == 0) {
        handle_sessions(fd);
    } else if (strcmp(buf, "version") == 0) {
        handle_version(fd);
    } else if (strcmp(buf, "stats") == 0) {
        handle_stats(fd);
    } else if (strcmp(buf, "help") == 0 || strcmp(buf, "?") == 0) {
        handle_help(fd);
    } else if (strlen(buf) > 0) {
        send_string_all(fd, "Unknown command. Type 'help' for available commands.\n");
    }
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

    if (!check_client_uid(fd)) {
        reactor_remove_fd(r, fd);
        close(fd);
        return;
    }

    char buf[UDS_BUFFER_SIZE];
    size_t total_read = 0;

    while (total_read < sizeof(buf) - 1) {
        ssize_t n = read(fd, buf + total_read, sizeof(buf) - 1 - total_read);
        if (n > 0) {
            total_read += (size_t)n;
        } else if (n == 0) {
            LOG_DEBUG("UDS: client closed connection (fd=%d)", fd);
            reactor_remove_fd(r, fd);
            close(fd);
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            LOG_ERROR("UDS: read error: %s", strerror(errno));
            reactor_remove_fd(r, fd);
            close(fd);
            return;
        }
    }

    if (total_read == 0) {
        return;
    }

    if (total_read >= sizeof(buf) - 1) {
        LOG_WARN("UDS: oversized request, closing connection");
        reactor_remove_fd(r, fd);
        close(fd);
        return;
    }

    buf[total_read] = '\0';

    char *cmd_start = buf;
    char *cmd_end;

    while (*cmd_start) {
        cmd_end = strchr(cmd_start, '\n');
        if (cmd_end) {
            *cmd_end = '\0';
            process_command(fd, cmd_start, (size_t)(cmd_end - cmd_start));
            cmd_start = cmd_end + 1;
        } else {
            process_command(fd, cmd_start, strlen(cmd_start));
            break;
        }
    }

    reactor_remove_fd(r, fd);
    close(fd);
}

/* ========== UDS Accept Callback ========== */

static void uds_accept_cb(reactor_t *r, int listen_fd, uint32_t events, void *userdata) {
    (void)r;
    (void)userdata;

    if (!(events & REACTOR_EVENT_READ)) return;

    while (1) {
        int client_fd = accept4(listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            LOG_ERROR("UDS: accept failed: %s", strerror(errno));
            break;
        }

        LOG_DEBUG("UDS: client connected (fd=%d)", client_fd);
        reactor_add_fd(r, client_fd, REACTOR_EVENT_READ | REACTOR_EVENT_EDGE,
                       uds_client_cb, NULL);
    }
}

/* ========== Init/Cleanup ========== */

int uds_init(reactor_t *r, const char *path, const uds_dependencies_t *deps) {
    struct sockaddr_un addr;
    size_t path_len;

    if (!r || !path || !deps) {
        return -1;
    }

    g_uds_dependencies = *deps;
    path_len = strlen(path);
    if (path_len >= sizeof(addr.sun_path)) {
        LOG_ERROR("UDS: socket path too long: %zu (max %zu)",
                  path_len, sizeof(addr.sun_path) - 1);
        return -1;
    }

    g_uds_reactor = r;
    g_uds_stop_requested = 0;

    strncpy(g_uds_path, path, UDS_PATH_MAX - 1);
    g_uds_path[UDS_PATH_MAX - 1] = '\0';

    unlink(path);

    g_uds_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (g_uds_fd < 0) {
        LOG_ERROR("UDS: socket failed: %s", strerror(errno));
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';

    if (bind(g_uds_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("UDS: bind to %s failed: %s", path, strerror(errno));
        close(g_uds_fd);
        g_uds_fd = -1;
        return -1;
    }

    if (chmod(path, 0600) < 0) {
        LOG_WARN("UDS: chmod 0600 failed: %s", strerror(errno));
    }

    if (listen(g_uds_fd, UDS_BACKLOG) < 0) {
        LOG_ERROR("UDS: listen failed: %s", strerror(errno));
        close(g_uds_fd);
        unlink(path);
        g_uds_fd = -1;
        return -1;
    }

    if (reactor_add_fd(r, g_uds_fd, REACTOR_EVENT_READ | REACTOR_EVENT_EDGE,
                       uds_accept_cb, NULL) != 0) {
        LOG_ERROR("UDS: reactor add failed");
        close(g_uds_fd);
        unlink(path);
        g_uds_fd = -1;
        return -1;
    }

    LOG_INFO("UDS: control socket created at %s (0600)", path);
    return 0;
}

void uds_cleanup(void) {
    if (g_uds_fd >= 0) {
        if (g_uds_reactor) {
            reactor_remove_fd(g_uds_reactor, g_uds_fd);
        }
        close(g_uds_fd);
        g_uds_fd = -1;
    }

    if (g_uds_path[0] != '\0') {
        unlink(g_uds_path);
        LOG_DEBUG("UDS: socket file removed: %s", g_uds_path);
        g_uds_path[0] = '\0';
    }

    g_uds_reactor = NULL;
    g_uds_stop_requested = 0;
    memset(&g_uds_dependencies, 0, sizeof(g_uds_dependencies));
}

int uds_get_fd(void) {
    return g_uds_fd;
}

int uds_stop_requested(void) {
    return g_uds_stop_requested;
}
