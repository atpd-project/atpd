#ifndef ATPD_SESSION_H
#define ATPD_SESSION_H

#include "reactor.h"
#include "atpd_context.h"
#include <sys/types.h>

#define ATPD_SESSION_PIPE_SIZE (64 * 1024)

/* ========== Error Codes ========== */

#define ATPD_SPLICE_OK            0
#define ATPD_SPLICE_EOF          -1
#define ATPD_SPLICE_EAGAIN       -2
#define ATPD_SPLICE_NOTSUP       -3
#define ATPD_SPLICE_ERROR        -4
#define ATPD_SPLICE_VPN_NOT_READY -5

typedef enum {
    ATPD_SESSION_IDLE,
    ATPD_SESSION_SPLICING,
    ATPD_SESSION_PIPE_DIRTY,
    ATPD_SESSION_CLOSING
} atpd_session_state_t;

typedef struct atpd_session {
    int fd_in;
    int fd_out;
    int pipe_fds[2];
    size_t pipe_pending;
    atpd_session_state_t state;
    reactor_t *reactor;
    uint64_t bytes_in;
    uint64_t bytes_out;
    uint64_t created_at;
    int ref_count;  /* Reference counter for UAF prevention */
} atpd_session_t;

atpd_session_t* atpd_session_create(reactor_t *r, int fd_in, int fd_out);
void atpd_session_destroy(atpd_session_t *s);
ssize_t atpd_session_splice_pump(atpd_session_t *s, size_t max_len);
int atpd_session_drain_pipe(atpd_session_t *s);
int atpd_session_register(reactor_t *r, atpd_session_t *s);

int atpd_session_is_vpn_ready(void);

ssize_t splice_pump_logic(atpd_session_t *s, size_t max_len);
void atpd_session_emergency_drain_all(void);

#endif
