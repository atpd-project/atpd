#ifndef ATPD_SPLICE_H
#define ATPD_SPLICE_H

#include <sys/types.h>
#include <stdbool.h>

/* Error codes */
#define ATPD_SPLICE_OK            0
#define ATPD_SPLICE_EOF          -1
#define ATPD_SPLICE_EAGAIN       -2
#define ATPD_SPLICE_NOTSUP       -3
#define ATPD_SPLICE_ERROR        -4

/* Splice state - must be maintained per connection */
typedef struct {
    int pipe_fds[2];
    size_t pipe_pending;
    size_t pipe_capacity;
    uint64_t bytes_in;
    uint64_t bytes_out;
    bool pipe_initialized;
} splice_state_t;

/* API Functions */
int atpd_splice_pipe_init(int pipe_fds[2]);
void atpd_splice_pipe_cleanup(int pipe_fds[2]);

/* State management */
int atpd_splice_state_init(splice_state_t *state);
void atpd_splice_state_cleanup(splice_state_t *state);

/**
 * Zero-copy forwarding using Dual-Splice pump with state management.
 *
 * Moves data from fd_in to fd_out through a kernel pipe buffer.
 * No data touches userspace memory.
 *
 * This is the RECOMMENDED API for production use with EPOLLET.
 *
 * @param fd_in     Source file descriptor (must support splice read)
 * @param fd_out    Destination file descriptor (must support splice write)
 * @param state     Splice state structure (must be per-connection)
 * @param max_len   Maximum bytes to forward (0 = no limit per call)
 *
 * @return Number of bytes successfully written to fd_out.
 *         Negative error code on failure:
 *           ATPD_SPLICE_OK (0)      - No data available
 *           ATPD_SPLICE_EAGAIN (-2) - Would block (normal for non-blocking)
 *           ATPD_SPLICE_EOF (-1)    - Connection closed
 *           ATPD_SPLICE_NOTSUP (-3) - splice not supported
 *           ATPD_SPLICE_ERROR (-4)  - Fatal error
 *
 * Note: total_forwarded counts only bytes successfully written to fd_out.
 *       bytes read from fd_in may be buffered in pipe (state->pipe_pending).
 */
ssize_t atpd_bridge_splice_stateful(int fd_in, int fd_out,
                                     splice_state_t *state,
                                     size_t max_len);

/**
 * Stateless splice API (deprecated).
 *
 * This version does NOT track pipe_pending and may not work correctly
 * with edge-triggered epoll under backpressure.
 *
 * DEPRECATED: Use atpd_bridge_splice_stateful instead.
 *
 * @deprecated Use atpd_bridge_splice_stateful with per-connection state.
 */
#if defined(__GNUC__)
__attribute__((deprecated("Use atpd_bridge_splice_stateful instead")))
#endif
ssize_t atpd_bridge_splice(int fd_in, int fd_out, int pipe_fds[2], size_t max_len);

#endif
