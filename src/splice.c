/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * Zero-copy forwarding using splice(2) - Dual-Splice pump
 * Compatible with EPOLLET edge-triggered epoll
 * State-aware with strict pipe_pending accounting
 * NO DATA LOSS on EAGAIN
 */

#include "splice.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
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
    state->pipe_initialized = false;

    if (pipe2(state->pipe_fds, O_NONBLOCK | O_CLOEXEC) < 0) {
        LOG_ERROR("[SPLICE] pipe2 failed: %s", strerror(errno));
        return -1;
    }

    int pipe_size = fcntl(state->pipe_fds[0], F_SETPIPE_SZ, ATPD_SPLICE_PIPE_SIZE);
    if (pipe_size < 0) {
        LOG_WARN("[SPLICE] F_SETPIPE_SZ failed: %s, using default", strerror(errno));
    } else {
        int actual = fcntl(state->pipe_fds[0], F_GETPIPE_SZ);
        LOG_DEBUG("[SPLICE] pipe size: requested=%d, actual=%d",
                  ATPD_SPLICE_PIPE_SIZE, actual);
    }

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
    state->pipe_initialized = false;

    LOG_DEBUG("[SPLICE] state cleaned up");
}

/* ========== Forwarding ========== */

static ssize_t splice_forward(splice_state_t *state,
                               int fd_in, int fd_out,
                               size_t max_len) {
    if (!state || !state->pipe_initialized || fd_in < 0 || fd_out < 0) {
        return ATPD_SPLICE_ERROR;
    }

    size_t total_forwarded = 0;
    size_t remaining_limit = max_len ? max_len : (size_t)-1;
    size_t chunk = ATPD_SPLICE_DEFAULT_CHUNK;

    /* Limit per-event to prevent reactor starvation */
    if (remaining_limit > ATPD_SPLICE_MAX_PER_EVENT) {
        remaining_limit = ATPD_SPLICE_MAX_PER_EVENT;
    }

    /* Debug invariant check */
#ifdef FCM_DEBUG
    assert(state->bytes_in >= state->bytes_out);
    assert(state->pipe_pending <= ATPD_SPLICE_PIPE_SIZE);
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
                    /* Output blocked - save exact pending state, DO NOT DRAIN */
                    state->pipe_pending = to_drain - drained;
                    if (drained > 0) {
                        state->bytes_out += drained;
                        return (ssize_t)drained;
                    }
                    return ATPD_SPLICE_EAGAIN;
                }

                if (errno == EPIPE || errno == ECONNRESET) {
                    state->pipe_pending = to_drain - drained;
                    return drained > 0 ? (ssize_t)drained : ATPD_SPLICE_EOF;
                }

                if (errno == EINVAL || errno == ENOSYS || errno == ESPIPE) {
                    LOG_DEBUG("[SPLICE] splice not supported for fd_out=%d", fd_out);
                    return ATPD_SPLICE_NOTSUP;
                }

                LOG_ERROR("[SPLICE] drain splice failed: %s", strerror(errno));
                return ATPD_SPLICE_ERROR;
            }

            if (n == 0) {
                state->pipe_pending = to_drain - drained;
                return drained > 0 ? (ssize_t)drained : ATPD_SPLICE_EOF;
            }

            drained += n;
            total_forwarded += n;
        }

        /* Strict accounting: subtract drained from pipe_pending */
        state->pipe_pending = to_drain - drained;
        state->bytes_out += drained;

        if (max_len && total_forwarded >= max_len) {
            return (ssize_t)total_forwarded;
        }

        if (max_len) {
            remaining_limit = (max_len > total_forwarded) ? (max_len - total_forwarded) : 0;
        }
        if (remaining_limit > ATPD_SPLICE_MAX_PER_EVENT) {
            remaining_limit = ATPD_SPLICE_MAX_PER_EVENT;
        }

        /* If pipe still has data, return and let caller retry */
        if (state->pipe_pending > 0) {
            return (ssize_t)total_forwarded;
        }
    }

    /* ============================================================
     * PHASE 2: Read from fd_in -> pipe (only if pipe empty)
     * ============================================================ */

    if (state->pipe_pending == 0 && remaining_limit > 0) {
        size_t read_chunk = chunk;
        if (read_chunk > remaining_limit) {
            read_chunk = remaining_limit;
        }

        LOG_DEBUG("[SPLICE] reading from fd_in: %zu bytes", read_chunk);

        ssize_t n = splice(fd_in, NULL, state->pipe_fds[1], NULL,
                           read_chunk, SPLICE_F_MOVE | SPLICE_F_NONBLOCK);

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* No data available from source */
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
            /* EOF from source */
            return total_forwarded > 0 ? (ssize_t)total_forwarded : ATPD_SPLICE_EOF;
        }

        /* Strict accounting: add to pipe_pending */
        state->pipe_pending += n;
        state->bytes_in += n;
    }

    /* ============================================================
     * PHASE 3: Write from pipe -> fd_out
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
                    /* Output blocked - save exact pending state, DO NOT DRAIN */
                    state->pipe_pending = to_write - written;
                    if (written > 0) {
                        total_forwarded += written;
                        state->bytes_out += total_forwarded;
                        return (ssize_t)total_forwarded;
                    }
                    return ATPD_SPLICE_EAGAIN;
                }

                if (errno == EPIPE || errno == ECONNRESET) {
                    state->pipe_pending = to_write - written;
                    if (written > 0) {
                        total_forwarded += written;
                        state->bytes_out += total_forwarded;
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
                state->pipe_pending = to_write - written;
                if (written > 0) {
                    total_forwarded += written;
                    state->bytes_out += total_forwarded;
                    return (ssize_t)total_forwarded;
                }
                return ATPD_SPLICE_EOF;
            }

            written += n;
            total_forwarded += n;
        }

        /* Strict accounting: subtract written from pipe_pending */
        state->pipe_pending = to_write - written;
        state->bytes_out += written;
    }

    if (max_len && total_forwarded >= max_len) {
        return (ssize_t)total_forwarded;
    }

    return total_forwarded > 0 ? (ssize_t)total_forwarded : ATPD_SPLICE_OK;
}

/* ========== Public API ========== */

/**
 * Set up pipe pair for zero-copy splicing.
 * Must be called before atpd_bridge_splice.
 * 
 * @param pipe_fds  Array of 2 ints to receive pipe FDs
 * @return 0 on success, -1 on error
 */
int atpd_splice_pipe_init(int pipe_fds[2]) {
    if (!pipe_fds) return -1;

    if (pipe2(pipe_fds, O_NONBLOCK | O_CLOEXEC) < 0) {
        LOG_ERROR("[SPLICE] pipe2 failed: %s", strerror(errno));
        return -1;
    }

    int pipe_size = fcntl(pipe_fds[0], F_SETPIPE_SZ, ATPD_SPLICE_PIPE_SIZE);
    if (pipe_size < 0) {
        LOG_WARN("[SPLICE] F_SETPIPE_SZ failed, using default: %s", strerror(errno));
    } else {
        int actual = fcntl(pipe_fds[0], F_GETPIPE_SZ);
        LOG_DEBUG("[SPLICE] pipe size: requested=%d, actual=%d",
                  ATPD_SPLICE_PIPE_SIZE, actual);
    }

    return 0;
}

/**
 * Zero-copy forwarding using Dual-Splice pump with state management.
 * Moves data from fd_in to fd_out through a kernel pipe buffer.
 * No data touches userspace memory.
 *
 * This implementation is STATE-AWARE and correctly handles pipe_pending
 * to support EPOLLET edge-triggered mode and backpressure.
 *
 * IMPORTANT: Caller must maintain pipe_state between calls.
 *
 * @param fd_in     Source file descriptor (must support splice read)
 * @param fd_out    Destination file descriptor (must support splice write)
 * @param pipe_fds  Pipe pair from atpd_splice_pipe_init
 * @param pipe_state  IN/OUT: state structure for this connection
 * @param max_len   Maximum bytes to forward (0 = unlimited until EAGAIN)
 * @return bytes forwarded on success, negative error code on failure
 */
ssize_t atpd_bridge_splice_stateful(int fd_in, int fd_out, int pipe_fds[2],
                                     splice_state_t *pipe_state,
                                     size_t max_len) {
    if (!pipe_fds || fd_in < 0 || fd_out < 0) {
        return ATPD_SPLICE_ERROR;
    }

    splice_state_t state;
    splice_state_t *state_ptr = pipe_state;

    if (!state_ptr) {
        /* Use local state if not provided (stateless mode) */
        memset(&state, 0, sizeof(state));
        state.pipe_fds[0] = pipe_fds[0];
        state.pipe_fds[1] = pipe_fds[1];
        state.pipe_initialized = true;
        state_ptr = &state;
    }

    return splice_forward(state_ptr, fd_in, fd_out, max_len);
}

/**
 * Zero-copy forwarding using Dual-Splice pump (stateless version).
 * DEPRECATED: Use atpd_bridge_splice_stateful for EPOLLET compatibility.
 *
 * This version does NOT track pipe_pending and may not work correctly
 * with edge-triggered epoll under backpressure.
 *
 * @deprecated Use atpd_bridge_splice_stateful with pipe_state tracking.
 */
ssize_t atpd_bridge_splice(int fd_in, int fd_out, int pipe_fds[2], size_t max_len) {
    if (!pipe_fds || fd_in < 0 || fd_out < 0) {
        return ATPD_SPLICE_ERROR;
    }

    /* Use local stateless fallback */
    splice_state_t local_state;
    memset(&local_state, 0, sizeof(local_state));
    local_state.pipe_fds[0] = pipe_fds[0];
    local_state.pipe_fds[1] = pipe_fds[1];
    local_state.pipe_initialized = true;

    return splice_forward(&local_state, fd_in, fd_out, max_len);
}

/**
 * Cleanup splice pipe FDs.
 * Safe to call with uninitialized pipe_fds (values < 0 are ignored).
 */
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
