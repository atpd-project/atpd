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

/* Stateless API (deprecated, use stateful version) */
ssize_t atpd_bridge_splice(int fd_in, int fd_out, int pipe_fds[2], size_t max_len);

/* Stateful API (recommended for EPOLLET) */
ssize_t atpd_bridge_splice_stateful(int fd_in, int fd_out, int pipe_fds[2],
                                     splice_state_t *pipe_state,
                                     size_t max_len);

#endif
