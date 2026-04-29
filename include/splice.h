#ifndef ATPD_SPLICE_H
#define ATPD_SPLICE_H

#include <sys/types.h>

#define ATPD_SPLICE_OK            0
#define ATPD_SPLICE_EOF          -1
#define ATPD_SPLICE_EAGAIN       -2
#define ATPD_SPLICE_NOTSUP       -3
#define ATPD_SPLICE_ERROR        -4

int atpd_splice_pipe_init(int pipe_fds[2]);
ssize_t atpd_bridge_splice(int fd_in, int fd_out, int pipe_fds[2], size_t max_len);
void atpd_splice_pipe_cleanup(int pipe_fds[2]);

#endif
