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

    LOG_DEBUG("session: destroying fd_in=%d fd_out=%d bytes_in=%" PRIu64 " bytes_out=%" PRIu64 "",
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
/* ========== Pipe Health Monitor ========== */

/**
 * check_pipe_health - Dynamically monitor and adjust pipe buffer size.
 *
 * Uses fcntl(F_GETPIPE_SZ) / fcntl(F_SETPIPE_SZ) to:
 * 1. Read the current kernel pipe buffer size
 * 2. If below target, attempt to increase to ATPD_SESSION_PIPE_SIZE
 * 3. If increase fails, report current value for diagnostics
 *
 * Returns current pipe size in bytes, or -1 on error.
 */
static int check_pipe_health(atpd_session_t *s) {
    if (!s || s->pipe_fds[0] < 0) return -1;

    int cur_size = fcntl(s->pipe_fds[0], F_GETPIPE_SZ);
    if (cur_size < 0) {
        LOG_WARN("session: F_GETPIPE_SZ failed for fd=%d: %s", s->pipe_fds[0], strerror(errno));
        return -1;
    }

    /* Only attempt resize if significantly below target (less than 75%) */
    if (cur_size < (ATPD_SESSION_PIPE_SIZE * 3 / 4)) {
        int new_size = fcntl(s->pipe_fds[0], F_SETPIPE_SZ, ATPD_SESSION_PIPE_SIZE);
        if (new_size < 0) {
            LOG_WARN("session: F_SETPIPE_SZ failed for fd=%d (current=%d, target=%d): %s",
                     s->pipe_fds[0], cur_size, ATPD_SESSION_PIPE_SIZE, strerror(errno));
            return cur_size;
        }
        LOG_INFO("session: pipe buffer resized %d -> %d bytes", cur_size, new_size);

        /* Sync write-end to same size */
        fcntl(s->pipe_fds[1], F_SETPIPE_SZ, new_size);
        return new_size;
    }

    return cur_size;
}

/* ========== Emergency Drain (XFRM_MSG_DELSA handler) ========== */

/**
 * emergency_drain - Rapidly purge pipe contents on VPN teardown.
 *
 * Called when XFRM_MSG_DELSA is received (VPN tunnel being destroyed).
 * This function:
 * 1. Reads all residual data from the pipe (discard it)
 * 2. Closes both pipe FDs to prevent further I/O
 * 3. Marks the session as CLOSING so Reactor will clean it up
 * 4. Logs the amount of data lost for auditing
 *
 * Must be called from the Reactor thread (single-threaded, no lock needed).
 */
static void emergency_drain(atpd_session_t *s) {
    if (!s) return;

    size_t drained = 0;
    char discard[4096];

    /* Phase 1: Drain all data from the read-end of the pipe */
    if (s->pipe_fds[0] > 0) {
        ssize_t n;
        while ((n = read(s->pipe_fds[0], discard, sizeof(discard))) > 0) {
            drained += (size_t)n;
        }
    }

    /* Phase 2: Drain any remaining data from the write-end */
    if (s->pipe_fds[1] > 0 && s->pipe_pending > 0) {
        /* Write-end has buffered data that will never be sent.
         * Close it immediately; the kernel will discard the buffer. */
        s->pipe_pending = 0;
    }

    /* Phase 3: Close pipe FDs to prevent further splice operations */
    if (s->pipe_fds[0] > 0) {
        close(s->pipe_fds[0]);
        s->pipe_fds[0] = -1;
    }
    if (s->pipe_fds[1] > 0) {
        close(s->pipe_fds[1]);
        s->pipe_fds[1] = -1;
    }

    /* Phase 4: Mark for Reactor cleanup */
    s->state = ATPD_SESSION_CLOSING;

    LOG_WARN("session: emergency drain completed (fd_in=%d, fd_out=%d, "
             "discarded=%zu bytes, pipe_pending=%zu)",
             s->fd_in, s->fd_out, drained, s->pipe_pending);
}

/* ========== Robust Splice Pump (EAGAIN/EINTR-safe) ========== */

/**
 * splice_pump_logic - Production-grade splice pump with full error handling.
 *
 * Design contract (C99, non-blocking):
 * - EAGAIN/EWOULDBLOCK: Return immediately with bytes_moved > 0 if partial progress,
 *   or ATPD_SPLICE_EAGAIN if no progress at all. Caller re-registers with epoll.
 * - EINTR: Retry the same operation immediately (SA_RESTART semantics not
 *   guaranteed for splice on all kernels).
 * - EPIPE/ECONNRESET: Remote closed, return ATPD_SPLICE_EOF.
 * - EINVAL: splice not supported for this FD type, return ATPD_SPLICE_NOTSUP.
 * - Other errors: Return ATPD_SPLICE_ERROR, caller should close the session.
 *
 * State management:
 * - On partial write (EAGAIN from pipe->fd_out), saves pipe_pending and sets
 *   state to ATPD_SESSION_PIPE_DIRTY for later drain.
 * - Calls check_pipe_health() before first splice to ensure optimal pipe size.
 */
ssize_t splice_pump_logic(atpd_session_t *s, size_t max_len) {
    if (!s) return ATPD_SPLICE_ERROR;

    /* VPN readiness gate */
    if (g_atpd_ctx.vpn_state != VPN_STATE_READY) {
        return ATPD_SPLICE_VPN_NOT_READY;
    }

    /* Periodic pipe health check (every ~64KB to avoid excessive syscalls) */
    static size_t health_check_counter = 0;
    if ((++health_check_counter & 0x3F) == 0) {  /* Every 64 calls */
        check_pipe_health(s);
    }

    ssize_t total_moved = 0;
    size_t remaining = max_len;

    while (remaining > 0) {
        /* === Phase 1: fd_in -> pipe (splice read) === */
        ssize_t moved = splice(s->fd_in, NULL, s->pipe_fds[1], NULL,
                               remaining, SPLICE_F_MOVE | SPLICE_F_NONBLOCK);

        if (moved < 0) {
            switch (errno) {
                case EAGAIN:
#if EAGAIN != EWOULDBLOCK
                case EWOULDBLOCK:
#endif
                    /* No more data available from source */
                    goto done;

                case EINTR:
                    /* Interrupted, retry immediately */
                    continue;

                case EPIPE:
                case ECONNRESET:
                    /* Remote closed */
                    if (total_moved > 0) goto done;
                    return ATPD_SPLICE_EOF;

                case EINVAL:
                    /* splice not supported for this FD */
                    LOG_ERROR("session: splice not supported for fd_in=%d", s->fd_in);
                    return ATPD_SPLICE_NOTSUP;

                default:
                    LOG_ERROR("session: splice read error fd_in=%d: %s", s->fd_in, strerror(errno));
                    return ATPD_SPLICE_ERROR;
            }
        }

        if (moved == 0) {
            /* EOF from source */
            if (total_moved > 0) goto done;
            return ATPD_SPLICE_EOF;
        }

        /* === Phase 2: pipe -> fd_out (splice write) === */
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
                        /* Output buffer full. Save state for later drain. */
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
                /* Output closed */
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

/* ========== VPN Teardown Handler (called from atpd_context_t kill-switch) ========== */

/**
 * atpd_session_emergency_drain_all - Emergency drain all registered sessions.
 *
 * Iterates the global session list and calls emergency_drain() on each.
 * This is the kill-switch callback registered in atpd_context_init().
 * Called from the Reactor thread when XFRM_MSG_DELSA is received.
 */
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
