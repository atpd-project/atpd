/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * Zero-copy forwarding using splice(2) - Dual-Splice pump
 * Compatible with EPOLLET edge-triggered epoll
 */

#include "reactor.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

/* ========== Constants ========== */

#define ATPD_SPLICE_PIPE_SIZE    (64 * 1024)   /* 64KB kernel buffer */
#define ATPD_SPLICE_DEFAULT_LEN  (64 * 1024)   /* Default splice chunk */
#define ATPD_SPLICE_MAX_RETRIES  3             /* EAGAIN retry limit */

/* ========== Error Codes ========== */

#define ATPD_SPLICE_OK            0
#define ATPD_SPLICE_EOF          -1   /* Connection closed */
#define ATPD_SPLICE_EAGAIN       -2   /* Would block (normal in non-blocking mode) */
#define ATPD_SPLICE_NOTSUP       -3   /* splice not supported for these FDs */
#define ATPD_SPLICE_ERROR        -4   /* Fatal error */

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
        LOG_ERROR("splice: pipe2 failed: %s", strerror(errno));
        return -1;
    }

    /* Set optimal pipe size for throughput */
    int pipe_size = fcntl(pipe_fds[0], F_SETPIPE_SZ, ATPD_SPLICE_PIPE_SIZE);
    if (pipe_size < 0) {
        LOG_WARN("splice: F_SETPIPE_SZ failed, using default: %s", strerror(errno));
    } else {
        LOG_DEBUG("splice: pipe buffer set to %d bytes", pipe_size);
    }

    return 0;
}

/**
 * Zero-copy forwarding using Dual-Splice pump.
 * Moves data from fd_in to fd_out through a kernel pipe buffer.
 * No data touches userspace memory.
 *
 * @param fd_in     Source file descriptor (must support splice read)
 * @param fd_out    Destination file descriptor (must support splice write)
 * @param pipe_fds  Pipe pair from atpd_splice_pipe_init
 * @param max_len   Maximum bytes to forward (0 = unlimited until EAGAIN)
 * @return bytes forwarded on success, negative error code on failure
 *         ATPD_SPLICE_OK (0) = no data available (EAGAIN on first call)
 *         ATPD_SPLICE_EOF (-1) = remote closed
 *         ATPD_SPLICE_EAGAIN (-2) = would block (normal edge-triggered behavior)
 *         ATPD_SPLICE_NOTSUP (-3) = splice not supported, use fallback
 *         ATPD_SPLICE_ERROR (-4) = fatal error, close connection
 */
ssize_t atpd_bridge_splice(int fd_in, int fd_out, int pipe_fds[2], size_t max_len) {
    if (!pipe_fds || fd_in < 0 || fd_out < 0) {
        return ATPD_SPLICE_ERROR;
    }

    size_t total_forwarded = 0;
    size_t chunk = max_len ? max_len : ATPD_SPLICE_DEFAULT_LEN;
    int retry = 0;

    while (1) {
        /* Phase 1: fd_in -> pipe (splice read) */
        ssize_t n = splice(fd_in, NULL, pipe_fds[1], NULL, chunk, SPLICE_F_MOVE | SPLICE_F_NONBLOCK);

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* No data available from source yet */
                if (total_forwarded == 0) {
                    return ATPD_SPLICE_OK;
                }
                /* Some data was forwarded, return what we have */
                return (ssize_t)total_forwarded;
            }

            if (errno == EINVAL) {
                /* splice not supported for this FD type */
                LOG_DEBUG("splice: not supported for fd_in=%d, use fallback", fd_in);
                return ATPD_SPLICE_NOTSUP;
            }

            if (errno == EPIPE || errno == ECONNRESET) {
                /* Remote closed */
                return ATPD_SPLICE_EOF;
            }

            LOG_ERROR("splice: read error on fd_in=%d: %s", fd_in, strerror(errno));
            return ATPD_SPLICE_ERROR;
        }

        if (n == 0) {
            /* EOF from source */
            if (total_forwarded == 0) {
                return ATPD_SPLICE_EOF;
            }
            return (ssize_t)total_forwarded;
        }

        /* Phase 2: pipe -> fd_out (splice write) */
        size_t remaining = (size_t)n;

        while (remaining > 0) {
            ssize_t m = splice(pipe_fds[0], NULL, fd_out, NULL, remaining,
                               SPLICE_F_MOVE | SPLICE_F_NONBLOCK);

            if (m < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    /* Output buffer full, try again */
                    retry++;
                    if (retry > ATPD_SPLICE_MAX_RETRIES) {
                        /* Too many retries, drain remaining pipe data and return */
                        char drain[4096];
                        while (read(pipe_fds[0], drain, sizeof(drain)) > 0);
                        return total_forwarded > 0 ? (ssize_t)total_forwarded : ATPD_SPLICE_EAGAIN;
                    }
                    continue;
                }

                if (errno == EINVAL) {
                    LOG_DEBUG("splice: not supported for fd_out=%d, use fallback", fd_out);
                    return ATPD_SPLICE_NOTSUP;
                }

                if (errno == EPIPE || errno == ECONNRESET) {
                    return total_forwarded > 0 ? (ssize_t)total_forwarded : ATPD_SPLICE_EOF;
                }

                LOG_ERROR("splice: write error on fd_out=%d: %s", fd_out, strerror(errno));
                return ATPD_SPLICE_ERROR;
            }

            if (m == 0) {
                return total_forwarded > 0 ? (ssize_t)total_forwarded : ATPD_SPLICE_EOF;
            }

            remaining -= (size_t)m;
            total_forwarded += (size_t)m;
            retry = 0;
        }

        /* Check if we've reached the max_len limit */
        if (max_len && total_forwarded >= max_len) {
            return (ssize_t)total_forwarded;
        }
    }
}

/**
 * Cleanup splice pipe FDs.
 * Safe to call with uninitialized pipe_fds (values <= 0 are ignored).
 */
void atpd_splice_pipe_cleanup(int pipe_fds[2]) {
    if (!pipe_fds) return;

    if (pipe_fds[0] > 0) {
        close(pipe_fds[0]);
        pipe_fds[0] = -1;
    }
    if (pipe_fds[1] > 0) {
        close(pipe_fds[1]);
        pipe_fds[1] = -1;
    }
}
