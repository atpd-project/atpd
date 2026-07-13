#include "session.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>

static void session_in_cb(reactor_t *r, int fd, uint32_t events, void *userdata);
static void session_out_cb(reactor_t *r, int fd, uint32_t events, void *userdata);
static void session_free_cb(void *userdata);

/* ========== Session Lifecycle ========== */

atpd_session_t* atpd_session_create(reactor_t *r, int fd_in, int fd_out) {
    atpd_session_t *s = calloc(1, sizeof(atpd_session_t));
    if (!s) return NULL;

    s->fd_in = fd_in;
    s->fd_out = fd_out;
    s->state = ATPD_SESSION_IDLE;
    s->reactor = r;
    s->created_at = reactor_now_ms();
    s->ref_count = 1;  /* Initial reference */

    if (pipe2(s->pipe_fds, O_NONBLOCK | O_CLOEXEC) < 0) {
        LOG_ERROR("session: pipe2 failed: %s", strerror(errno));
        free(s);
        return NULL;
    }

    fcntl(s->pipe_fds[0], F_SETPIPE_SZ, ATPD_SESSION_PIPE_SIZE);
    fcntl(s->pipe_fds[1], F_SETPIPE_SZ, ATPD_SESSION_PIPE_SIZE);

    atpd_session_register_to_ctx(s);

    LOG_DEBUG("session: created fd_in=%d fd_out=%d ref=1", fd_in, fd_out);
    return s;
}

static void session_ref(atpd_session_t *s) {
    if (s) {
        s->ref_count++;
        LOG_DEBUG("session: ref++ fd_in=%d ref=%d", s->fd_in, s->ref_count);
    }
}

static void session_unref(atpd_session_t *s) {
    if (!s) return;
    s->ref_count--;
    LOG_DEBUG("session: ref-- fd_in=%d ref=%d", s->fd_in, s->ref_count);
    if (s->ref_count == 0) {
        LOG_DEBUG("session: refcount zero, destroying fd_in=%d", s->fd_in);
        atpd_session_destroy(s);
    }
}

void atpd_session_destroy(atpd_session_t *s) {
    if (!s) return;

    /* Prevent double destroy */
    if (s->state == ATPD_SESSION_CLOSING) {
        LOG_DEBUG("session: already closing fd_in=%d", s->fd_in);
        return;
    }

    LOG_DEBUG("session: destroying fd_in=%d fd_out=%d bytes_in=%" PRIu64 " bytes_out=%" PRIu64 "",
              s->fd_in, s->fd_out, s->bytes_in, s->bytes_out);

    s->state = ATPD_SESSION_CLOSING;

    atpd_session_unregister_from_ctx(s);

    if (s->pipe_fds[0] > 0) {
        close(s->pipe_fds[0]);
        s->pipe_fds[0] = -1;
    }
    if (s->pipe_fds[1] > 0) {
        close(s->pipe_fds[1]);
        s->pipe_fds[1] = -1;
    }

    if (s->reactor) {
        reactor_remove_fd(s->reactor, s->fd_in);
        reactor_remove_fd(s->reactor, s->fd_out);
    }

    if (s->fd_in > 0) {
        close(s->fd_in);
        s->fd_in = -1;
    }
    if (s->fd_out > 0) {
        close(s->fd_out);
        s->fd_out = -1;
    }

    s->reactor = NULL;
    free(s);
}

/* ========== VPN State ========== */

int atpd_session_is_vpn_ready(void) {
    return g_atpd_ctx.vpn_state == VPN_STATE_READY;
}

/* ========== Splice Pump ========== */

ssize_t atpd_session_splice_pump(atpd_session_t *s, size_t max_len) {
    if (!s) return ATPD_SPLICE_ERROR;

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

/* ========== Pipe Drain ========== */

int atpd_session_drain_pipe(atpd_session_t *s) {
    if (!s) return -1;
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

/* ========== Session Registration ========== */

int atpd_session_register(reactor_t *r, atpd_session_t *s) {
    if (!s) return -1;

    /* Hold reference for each callback */
    session_ref(s);
    reactor_add_fd_ex(r, s->fd_in, REACTOR_EVENT_READ | REACTOR_EVENT_EDGE,
                      session_in_cb, session_free_cb, s);

    session_ref(s);
    reactor_add_fd_ex(r, s->fd_out, REACTOR_EVENT_READ | REACTOR_EVENT_EDGE,
                      session_out_cb, NULL, s);

    s->state = ATPD_SESSION_SPLICING;
    return 0;
}

/* ========== IN Callback ========== */

static void session_in_cb(reactor_t *r, int fd, uint32_t events, void *userdata) {
    (void)r;
    atpd_session_t *s = userdata;

    if (!s) return;

    /* Take reference for this callback */
    session_ref(s);

    if (events & (REACTOR_EVENT_ERROR | REACTOR_EVENT_HANGUP)) {
        s->state = ATPD_SESSION_CLOSING;
        session_unref(s);
        return;
    }

    if (s->state == ATPD_SESSION_PIPE_DIRTY) {
        atpd_session_drain_pipe(s);
        if (s->state == ATPD_SESSION_PIPE_DIRTY) {
            session_unref(s);
            return;
        }
    }

    ssize_t ret = atpd_session_splice_pump(s, ATPD_SESSION_PIPE_SIZE);
    if (ret == ATPD_SPLICE_VPN_NOT_READY) {
        session_unref(s);
        return;
    }
    if (ret < 0) {
        LOG_ERROR("session: splice pump failed, closing");
        s->state = ATPD_SESSION_CLOSING;
        session_unref(s);
        return;
    }

    session_unref(s);
}

/* ========== OUT Callback ========== */

static void session_out_cb(reactor_t *r, int fd, uint32_t events, void *userdata) {
    (void)r;
    atpd_session_t *s = userdata;

    if (!s) return;

    /* Take reference for this callback */
    session_ref(s);

    if (events & (REACTOR_EVENT_ERROR | REACTOR_EVENT_HANGUP)) {
        s->state = ATPD_SESSION_CLOSING;
        session_unref(s);
        return;
    }

    if (s->state == ATPD_SESSION_PIPE_DIRTY) {
        atpd_session_drain_pipe(s);
        if (s->state == ATPD_SESSION_PIPE_DIRTY) {
            session_unref(s);
            return;
        }

        reactor_modify_fd(r, s->fd_in, REACTOR_EVENT_READ | REACTOR_EVENT_EDGE);
    }

    session_unref(s);
}

/* ========== Free Callback ========== */

static void session_free_cb(void *userdata) {
    atpd_session_t *s = userdata;
    if (!s) return;

    LOG_DEBUG("session: free_cb called fd_in=%d", s->fd_in);
    session_unref(s);
}

/* ========== Pipe Health Monitor ========== */

static int check_pipe_health(atpd_session_t *s) {
    if (!s || s->pipe_fds[0] < 0) return -1;

    int cur_size = fcntl(s->pipe_fds[0], F_GETPIPE_SZ);
    if (cur_size < 0) {
        LOG_WARN("session: F_GETPIPE_SZ failed for fd=%d: %s", s->pipe_fds[0], strerror(errno));
        return -1;
    }

    if (cur_size < (ATPD_SESSION_PIPE_SIZE * 3 / 4)) {
        int new_size = fcntl(s->pipe_fds[0], F_SETPIPE_SZ, ATPD_SESSION_PIPE_SIZE);
        if (new_size < 0) {
            LOG_WARN("session: F_SETPIPE_SZ failed for fd=%d (current=%d, target=%d): %s",
                     s->pipe_fds[0], cur_size, ATPD_SESSION_PIPE_SIZE, strerror(errno));
            return cur_size;
        }
        LOG_INFO("session: pipe buffer resized %d -> %d bytes", cur_size, new_size);
        fcntl(s->pipe_fds[1], F_SETPIPE_SZ, new_size);
        return new_size;
    }

    return cur_size;
}

/* ========== Emergency Drain ========== */

static void emergency_drain(atpd_session_t *s) {
    if (!s) return;

    size_t drained = 0;
    char discard[4096];

    if (s->pipe_fds[0] > 0) {
        ssize_t n;
        while ((n = read(s->pipe_fds[0], discard, sizeof(discard))) > 0) {
            drained += (size_t)n;
        }
    }

    if (s->pipe_fds[1] > 0 && s->pipe_pending > 0) {
        s->pipe_pending = 0;
    }

    if (s->pipe_fds[0] > 0) {
        close(s->pipe_fds[0]);
        s->pipe_fds[0] = -1;
    }
    if (s->pipe_fds[1] > 0) {
        close(s->pipe_fds[1]);
        s->pipe_fds[1] = -1;
    }

    s->state = ATPD_SESSION_CLOSING;

    LOG_WARN("session: emergency drain completed (fd_in=%d, fd_out=%d, "
             "discarded=%zu bytes, pipe_pending=%zu)",
             s->fd_in, s->fd_out, drained, s->pipe_pending);
}

/* ========== Splice Pump Logic ========== */

ssize_t splice_pump_logic(atpd_session_t *s, size_t max_len) {
    if (!s) return ATPD_SPLICE_ERROR;

    if (g_atpd_ctx.vpn_state != VPN_STATE_READY) {
        return ATPD_SPLICE_VPN_NOT_READY;
    }

    static size_t health_check_counter = 0;
    if ((++health_check_counter & 0x3F) == 0) {
        check_pipe_health(s);
    }

    ssize_t total_moved = 0;
    size_t remaining = max_len;

    while (remaining > 0) {
        ssize_t moved = splice(s->fd_in, NULL, s->pipe_fds[1], NULL,
                               remaining, SPLICE_F_MOVE | SPLICE_F_NONBLOCK);

        if (moved < 0) {
            switch (errno) {
                case EAGAIN:
#if EAGAIN != EWOULDBLOCK
                case EWOULDBLOCK:
#endif
                    goto done;

                case EINTR:
                    continue;

                case EPIPE:
                case ECONNRESET:
                    if (total_moved > 0) goto done;
                    return ATPD_SPLICE_EOF;

                case EINVAL:
                    LOG_ERROR("session: splice not supported for fd_in=%d", s->fd_in);
                    return ATPD_SPLICE_NOTSUP;

                default:
                    LOG_ERROR("session: splice read error fd_in=%d: %s", s->fd_in, strerror(errno));
                    return ATPD_SPLICE_ERROR;
            }
        }

        if (moved == 0) {
            if (total_moved > 0) goto done;
            return ATPD_SPLICE_EOF;
        }

        ssize_t sent = 0;
        size_t to_send = (size_t)moved;

        while (sent < (ssize_t)to_send) {
            ssize_t ret = splice(s->pipe_fds[0], NULL, s->fd_out, NULL,
                                 to_send - (size_t)sent,
                                 SPLICE_F_MOVE | SPLICE_F_NONBLOCK);

            if (ret < 0) {
                switch (errno) {
                    case EAGAIN:
#if EAGAIN != EWOULDBLOCK
                    case EWOULDBLOCK:
#endif
                        s->pipe_pending = to_send - (size_t)sent;
                        s->state = ATPD_SESSION_PIPE_DIRTY;
                        s->bytes_in += (uint64_t)sent;
                        s->bytes_out += (uint64_t)total_moved + (uint64_t)sent;
                        return total_moved + sent;

                    case EINTR:
                        continue;

                    case EPIPE:
                    case ECONNRESET:
                        s->pipe_pending = to_send - (size_t)sent;
                        s->state = ATPD_SESSION_PIPE_DIRTY;
                        if (total_moved + sent > 0) return total_moved + sent;
                        return ATPD_SPLICE_EOF;

                    default:
                        LOG_ERROR("session: splice write error fd_out=%d: %s", s->fd_out, strerror(errno));
                        return ATPD_SPLICE_ERROR;
                }
            }

            if (ret == 0) {
                if (total_moved + sent > 0) return total_moved + sent;
                return ATPD_SPLICE_EOF;
            }

            sent += ret;
        }

        total_moved += sent;
        remaining -= (size_t)sent;
    }

done:
    s->bytes_in += (uint64_t)total_moved;
    s->bytes_out += (uint64_t)total_moved;
    return total_moved > 0 ? total_moved : ATPD_SPLICE_OK;
}

/* ========== Emergency Drain All ========== */

void atpd_session_emergency_drain_all(void) {
    struct atpd_session_list *node = g_atpd_ctx.sessions;
    int drained = 0;

    while (node) {
        struct atpd_session *s = node->session;
        if (s && s->state != ATPD_SESSION_CLOSING) {
            emergency_drain(s);
            drained++;
        }
        node = node->next;
    }

    LOG_WARN("session: emergency drain completed for %d sessions", drained);
}
