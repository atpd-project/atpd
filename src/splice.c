/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * Zero-copy forwarding using splice(2) - Dual-Splice pump
 * Compatible with EPOLLET edge-triggered epoll
 * State-aware with strict pipe_pending accounting
 * NO DATA LOSS on EAGAIN
 *
 * Production Ready - Release Approved
 */

#include "splice.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <assert.h>

/* ========== Constants ========== */

#define ATPD_SPLICE_PIPE_SIZE       (64 * 1024)
#define ATPD_SPLICE_DEFAULT_CHUNK   (64 * 1024)
#define ATPD_SPLICE_MAX_PER_EVENT   (4 * 1024 * 1024)

/* ========== State Management ========== */

int atpd_splice_state_init(splice_state_t *state) {
    if (!state) return -1;

    memset(state, 0, sizeof(splice_state_t));
    state->pipe_fds[0] = -1;
    state->pipe_fds[1] = -1;
    state->pipe_pending = 0;
    state->pipe_capacity = 0;
    state->pipe_initialized = false;

    if (pipe2(state->pipe_fds, O_NONBLOCK | O_CLOEXEC) < 0) {
        LOG_ERROR("[SPLICE] pipe2 failed: %s", strerror(errno));
        return -1;
    }

    int requested = ATPD_SPLICE_PIPE_SIZE;
    int actual = fcntl(state->pipe_fds[0], F_SETPIPE_SZ, requested);
    if (actual < 0) {
        LOG_WARN("[SPLICE] F_SETPIPE_SZ failed: %s, using default", strerror(errno));
        actual = fcntl(state->pipe_fds[0], F_GETPIPE_SZ);
        if (actual < 0) {
            actual = requested;
        }
    }

    state->pipe_capacity = (size_t)actual;
    LOG_DEBUG("[SPLICE] pipe size: requested=%d, actual=%zu",
              requested, state->pipe_capacity);

    state->pipe_initialized = true;
    LOG_DEBUG("[SPLICE] state initialized, pipe_fds=[%d,%d]",
              state->pipe_fds[0], state->pipe_fds[1]);
    return 0;
}

void atpd_splice_state_cleanup(splice_state_t *state) {
    if (!state) return;

    if (state->pipe_fds[0] >= 0) {
        close(state->pipe_fds[0]);
        state->pipe_fds[0] = -1;
    }
    if (state->pipe_fds[1] >= 0) {
        close(state->pipe_fds[1]);
        state->pipe_fds[1] = -1;
    }

    state->pipe_pending = 0;
    state->pipe_capacity = 0;
    state->pipe_initialized = false;
    state->bytes_in = 0;
    state->bytes_out = 0;

    LOG_DEBUG("[SPLICE] state cleaned up");
}

/* ========== Forwarding ========== */

ssize_t atpd_bridge_splice_stateful(int fd_in, int fd_out,
                                     splice_state_t *state,
                                     size_t max_len) {
    if (!state || !state->pipe_initialized || fd_in < 0 || fd_out < 0) {
        return ATPD_SPLICE_ERROR;
    }

    size_t total_forwarded = 0;
    size_t remaining_limit = max_len ? max_len : (size_t)-1;
    size_t chunk = ATPD_SPLICE_DEFAULT_CHUNK;

    if (remaining_limit > ATPD_SPLICE_MAX_PER_EVENT) {
        remaining_limit = ATPD_SPLICE_MAX_PER_EVENT;
    }

#ifdef ATPD_DEBUG
    /* Verify pipe_pending consistency with kernel */
    int kernel_pending = 0;
    if (ioctl(state->pipe_fds[0], FIONREAD, &kernel_pending) == 0) {
        assert((size_t)kernel_pending == state->pipe_pending);
    }

    /* Invariant checks */
    assert(state->pipe_fds[0] >= 0);
    assert(state->pipe_fds[1] >= 0);
    assert(state->bytes_in >= state->bytes_out);
    assert(state->pipe_pending <= state->pipe_capacity);
    assert(state->bytes_in == state->bytes_out + state->pipe_pending);
#endif

    /* ============================================================
     * PHASE 1: Drain pipe pending data first
     * DO NOT read from fd_in until pipe is empty
     * ============================================================ */

    if (state->pipe_pending > 0) {
        size_t to_drain = state->pipe_pending;
        if (to_drain > remaining_limit) {
            to_drain = remaining_limit;
        }

        LOG_DEBUG("[SPLICE] draining pipe pending: %zu bytes", to_drain);

        ssize_t drained = 0;
        while (drained < (ssize_t)to_drain) {
            ssize_t n = splice(state->pipe_fds[0], NULL, fd_out, NULL,
                               to_drain - drained,
                               SPLICE_F_MOVE | SPLICE_F_NONBLOCK);

            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    if (drained > 0) {
                        state->pipe_pending -= drained;
                        state->bytes_out += drained;
                        return (ssize_t)drained;
                    }
                    return ATPD_SPLICE_EAGAIN;
                }

                if (errno == EPIPE || errno == ECONNRESET) {
                    if (drained > 0) {
                        state->pipe_pending -= drained;
                        state->bytes_out += drained;
                        return (ssize_t)drained;
                    }
                    return ATPD_SPLICE_EOF;
                }

                if (errno == EINVAL || errno == ENOSYS || errno == ESPIPE) {
                    LOG_DEBUG("[SPLICE] splice not supported for fd_out=%d", fd_out);
                    return ATPD_SPLICE_NOTSUP;
                }

                LOG_ERROR("[SPLICE] drain splice failed: %s", strerror(errno));
                return ATPD_SPLICE_ERROR;
            }

            if (n == 0) {
                if (drained > 0) {
                    state->pipe_pending -= drained;
                    state->bytes_out += drained;
                    return (ssize_t)drained;
                }
                return ATPD_SPLICE_EOF;
            }

            drained += n;
        }

        state->pipe_pending -= drained;
        state->bytes_out += drained;

        if (max_len && drained >= max_len) {
            return (ssize_t)drained;
        }

        if (max_len) {
            remaining_limit = (max_len > drained) ? (max_len - drained) : 0;
        }
        if (remaining_limit > ATPD_SPLICE_MAX_PER_EVENT) {
            remaining_limit = ATPD_SPLICE_MAX_PER_EVENT;
        }

        if (state->pipe_pending > 0) {
            return (ssize_t)drained;
        }

        total_forwarded = drained;
    }

    /* ============================================================
     * PHASE 2: Read from fd_in -> pipe (only if pipe empty)
     * total_forwarded NOT updated here - data only enters pipe
     * ============================================================ */

    if (state->pipe_pending == 0 && remaining_limit > 0) {
        size_t read_chunk = chunk;
        if (read_chunk > remaining_limit) {
            read_chunk = remaining_limit;
        }
        if (read_chunk > state->pipe_capacity) {
            read_chunk = state->pipe_capacity;
        }

        LOG_DEBUG("[SPLICE] reading from fd_in: %zu bytes", read_chunk);

        ssize_t n = splice(fd_in, NULL, state->pipe_fds[1], NULL,
                           read_chunk, SPLICE_F_MOVE | SPLICE_F_NONBLOCK);

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return total_forwarded > 0 ? (ssize_t)total_forwarded : ATPD_SPLICE_EAGAIN;
            }

            if (errno == EINVAL || errno == ENOSYS || errno == ESPIPE) {
                LOG_DEBUG("[SPLICE] splice not supported for fd_in=%d", fd_in);
                return ATPD_SPLICE_NOTSUP;
            }

            if (errno == EPIPE || errno == ECONNRESET) {
                return total_forwarded > 0 ? (ssize_t)total_forwarded : ATPD_SPLICE_EOF;
            }

            LOG_ERROR("[SPLICE] read splice failed: %s", strerror(errno));
            return ATPD_SPLICE_ERROR;
        }

        if (n == 0) {
            return total_forwarded > 0 ? (ssize_t)total_forwarded : ATPD_SPLICE_EOF;
        }

        /* Data enters pipe - update pipe_pending and bytes_in only */
        state->pipe_pending += n;
        state->bytes_in += n;
        /* total_forwarded NOT incremented here */
    }

    /* ============================================================
     * PHASE 3: Write from pipe -> fd_out
     * total_forwarded updated here - data actually leaves pipe
     * ============================================================ */

    if (state->pipe_pending > 0) {
        size_t to_write = state->pipe_pending;
        if (to_write > remaining_limit) {
            to_write = remaining_limit;
        }

        LOG_DEBUG("[SPLICE] writing to fd_out: %zu bytes", to_write);

        ssize_t written = 0;
        while (written < (ssize_t)to_write) {
            ssize_t n = splice(state->pipe_fds[0], NULL, fd_out, NULL,
                               to_write - written,
                               SPLICE_F_MOVE | SPLICE_F_NONBLOCK);

            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    if (written > 0) {
                        state->pipe_pending -= written;
                        state->bytes_out += written;
                        total_forwarded += written;
                        return (ssize_t)total_forwarded;
                    }
                    return ATPD_SPLICE_EAGAIN;
                }

                if (errno == EPIPE || errno == ECONNRESET) {
                    if (written > 0) {
                        state->pipe_pending -= written;
                        state->bytes_out += written;
                        total_forwarded += written;
                        return (ssize_t)total_forwarded;
                    }
                    return ATPD_SPLICE_EOF;
                }

                if (errno == EINVAL || errno == ENOSYS || errno == ESPIPE) {
                    LOG_DEBUG("[SPLICE] splice not supported for fd_out=%d", fd_out);
                    return ATPD_SPLICE_NOTSUP;
                }

                LOG_ERROR("[SPLICE] write splice failed: %s", strerror(errno));
                return ATPD_SPLICE_ERROR;
            }

            if (n == 0) {
                if (written > 0) {
                    state->pipe_pending -= written;
                    state->bytes_out += written;
                    total_forwarded += written;
                    return (ssize_t)total_forwarded;
                }
                return ATPD_SPLICE_EOF;
            }

            written += n;
        }

        state->pipe_pending -= written;
        state->bytes_out += written;
        total_forwarded += written;
    }

    if (max_len && total_forwarded >= max_len) {
        return (ssize_t)total_forwarded;
    }

    return total_forwarded > 0 ? (ssize_t)total_forwarded : ATPD_SPLICE_OK;
}

/* ========== Legacy API (Deprecated) ========== */

ssize_t atpd_bridge_splice(int fd_in, int fd_out, int pipe_fds[2], size_t max_len) {
    if (!pipe_fds || fd_in < 0 || fd_out < 0) {
        return ATPD_SPLICE_ERROR;
    }

    static int warned = 0;
    if (!warned) {
        LOG_WARN("[SPLICE] atpd_bridge_splice() is deprecated. "
                 "Use atpd_bridge_splice_stateful() for EPOLLET support.");
        warned = 1;
    }

    splice_state_t local_state;
    memset(&local_state, 0, sizeof(local_state));
    local_state.pipe_fds[0] = pipe_fds[0];
    local_state.pipe_fds[1] = pipe_fds[1];
    local_state.pipe_initialized = true;
    local_state.pipe_capacity = ATPD_SPLICE_PIPE_SIZE;

    return atpd_bridge_splice_stateful(fd_in, fd_out, &local_state, max_len);
}

/* ========== Pipe Utilities ========== */

int atpd_splice_pipe_init(int pipe_fds[2]) {
    if (!pipe_fds) return -1;

    if (pipe2(pipe_fds, O_NONBLOCK | O_CLOEXEC) < 0) {
        LOG_ERROR("[SPLICE] pipe2 failed: %s", strerror(errno));
        return -1;
    }

    int requested = ATPD_SPLICE_PIPE_SIZE;
    int actual = fcntl(pipe_fds[0], F_SETPIPE_SZ, requested);
    if (actual < 0) {
        LOG_WARN("[SPLICE] F_SETPIPE_SZ failed, using default: %s", strerror(errno));
        actual = fcntl(pipe_fds[0], F_GETPIPE_SZ);
        if (actual < 0) {
            actual = requested;
        }
    }

    LOG_DEBUG("[SPLICE] pipe size: requested=%d, actual=%d", requested, actual);
    return 0;
}

void atpd_splice_pipe_cleanup(int pipe_fds[2]) {
    if (!pipe_fds) return;

    if (pipe_fds[0] >= 0) {
        close(pipe_fds[0]);
        pipe_fds[0] = -1;
    }
    if (pipe_fds[1] >= 0) {
        close(pipe_fds[1]);
        pipe_fds[1] = -1;
    }
}
