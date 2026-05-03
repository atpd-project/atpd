#include "session.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

static void session_in_cb(reactor_t *r, int fd, uint32_t events, void *userdata);
static void session_out_cb(reactor_t *r, int fd, uint32_t events, void *userdata);
static void session_free_cb(void *userdata);

atpd_session_t* atpd_session_create(reactor_t *r, int fd_in, int fd_out) {
    atpd_session_t *s = calloc(1, sizeof(atpd_session_t));
    if (!s) return NULL;

    s->fd_in = fd_in;
    s->fd_out = fd_out;
    s->state = ATPD_SESSION_IDLE;
    s->reactor = r;
    s->created_at = reactor_now_ms();

    if (pipe2(s->pipe_fds, O_NONBLOCK | O_CLOEXEC) < 0) {
        LOG_ERROR("session: pipe2 failed: %s", strerror(errno));
        free(s);
        return NULL;
    }

    fcntl(s->pipe_fds[0], F_SETPIPE_SZ, ATPD_SESSION_PIPE_SIZE);
    fcntl(s->pipe_fds[1], F_SETPIPE_SZ, ATPD_SESSION_PIPE_SIZE);

    /* Register with global context for kill-switch */
    atpd_session_register_to_ctx(s);

    LOG_DEBUG("session: created fd_in=%d fd_out=%d", fd_in, fd_out);
    return s;
}

void atpd_session_destroy(atpd_session_t *s) {
    if (!s) return;

    LOG_DEBUG("session: destroying fd_in=%d fd_out=%d bytes_in=%llu bytes_out=%llu",
              s->fd_in, s->fd_out, s->bytes_in, s->bytes_out);

    /* Unregister from global context */
    atpd_session_unregister_from_ctx(s);

    if (s->pipe_fds[0] > 0) close(s->pipe_fds[0]);
    if (s->pipe_fds[1] > 0) close(s->pipe_fds[1]);

    reactor_remove_fd(s->reactor, s->fd_in);
    reactor_remove_fd(s->reactor, s->fd_out);
    close(s->fd_in);
    close(s->fd_out);

    free(s);
}

int atpd_session_is_vpn_ready(void) {
    return g_atpd_ctx.vpn_state == VPN_STATE_READY;
}

ssize_t atpd_session_splice_pump(atpd_session_t *s, size_t max_len) {
    /* Gate: only splice when VPN is READY */
    if (!atpd_session_is_vpn_ready()) {
        LOG_DEBUG("session: splice blocked, VPN not ready (state=%s)",
                  vpn_state_string(g_atpd_ctx.vpn_state));
        return ATPD_SPLICE_VPN_NOT_READY;
    }

    ssize_t total_moved = 0;

    while (total_moved < (ssize_t)max_len) {
        ssize_t moved = splice(s->fd_in, NULL, s->pipe_fds[1], NULL,
                               max_len - total_moved,
                               SPLICE_F_MOVE | SPLICE_F_NONBLOCK);

        if (moved < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            return ATPD_SPLICE_ERROR;
        }
        if (moved == 0) break;

        ssize_t sent = 0;
        while (sent < moved) {
            ssize_t ret = splice(s->pipe_fds[0], NULL, s->fd_out, NULL,
                                 moved - sent,
                                 SPLICE_F_MOVE | SPLICE_F_NONBLOCK);

            if (ret < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    s->pipe_pending = moved - sent;
                    s->state = ATPD_SESSION_PIPE_DIRTY;
                    s->bytes_in += (uint64_t)sent;
                    s->bytes_out += (uint64_t)total_moved + sent;
                    return total_moved + sent;
                }
                if (errno == EINTR) continue;
                return ATPD_SPLICE_ERROR;
            }
            if (ret == 0) break;
            sent += ret;
        }
        total_moved += sent;
    }

    s->bytes_in += (uint64_t)total_moved;
    s->bytes_out += (uint64_t)total_moved;
    return total_moved;
}

int atpd_session_drain_pipe(atpd_session_t *s) {
    if (s->state != ATPD_SESSION_PIPE_DIRTY || s->pipe_pending == 0) return 0;

    ssize_t sent = 0;
    while (sent < (ssize_t)s->pipe_pending) {
        ssize_t ret = splice(s->pipe_fds[0], NULL, s->fd_out, NULL,
                             s->pipe_pending - sent,
                             SPLICE_F_MOVE | SPLICE_F_NONBLOCK);

        if (ret < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                s->pipe_pending -= sent;
                return 1;
            }
            if (errno == EINTR) continue;
            return -1;
        }
        if (ret == 0) break;
        sent += ret;
    }

    s->pipe_pending = 0;
    s->state = ATPD_SESSION_SPLICING;
    return 0;
}

int atpd_session_register(reactor_t *r, atpd_session_t *s) {
    reactor_add_fd_ex(r, s->fd_in, REACTOR_EVENT_READ | REACTOR_EVENT_EDGE,
                      session_in_cb, session_free_cb, s);
    reactor_add_fd_ex(r, s->fd_out, REACTOR_EVENT_READ | REACTOR_EVENT_EDGE,
                      session_out_cb, NULL, s);
    s->state = ATPD_SESSION_SPLICING;
    return 0;
}

static void session_in_cb(reactor_t *r, int fd, uint32_t events, void *userdata) {
    (void)r;
    atpd_session_t *s = userdata;

    if (events & (REACTOR_EVENT_ERROR | REACTOR_EVENT_HANGUP)) {
        s->state = ATPD_SESSION_CLOSING;
        atpd_session_destroy(s);
        return;
    }

    if (s->state == ATPD_SESSION_PIPE_DIRTY) {
        atpd_session_drain_pipe(s);
        if (s->state == ATPD_SESSION_PIPE_DIRTY) return;
    }

    ssize_t ret = atpd_session_splice_pump(s, ATPD_SESSION_PIPE_SIZE);
    if (ret == ATPD_SPLICE_VPN_NOT_READY) {
        return;
    }
    if (ret < 0) {
        LOG_ERROR("session: splice pump failed, closing");
        s->state = ATPD_SESSION_CLOSING;
        atpd_session_destroy(s);
    }
}

static void session_out_cb(reactor_t *r, int fd, uint32_t events, void *userdata) {
    (void)r;
    atpd_session_t *s = userdata;

    if (events & (REACTOR_EVENT_ERROR | REACTOR_EVENT_HANGUP)) {
        s->state = ATPD_SESSION_CLOSING;
        atpd_session_destroy(s);
        return;
    }

    if (s->state == ATPD_SESSION_PIPE_DIRTY) {
        atpd_session_drain_pipe(s);
        if (s->state == ATPD_SESSION_PIPE_DIRTY) return;

        reactor_modify_fd(r, s->fd_in, REACTOR_EVENT_READ | REACTOR_EVENT_EDGE);
    }
}

static void session_free_cb(void *userdata) {
    atpd_session_t *s = userdata;
    if (s && s->state != ATPD_SESSION_CLOSING) {
        s->state = ATPD_SESSION_CLOSING;
        atpd_session_destroy(s);
    }
}
