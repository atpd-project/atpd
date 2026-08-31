#ifndef ATPD_SESSION_H
#define ATPD_SESSION_H

#include "reactor.h"
#include "atpd_context.h"
#include <sys/types.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

#define ATPD_SESSION_PIPE_SIZE (64 * 1024)

/* ========== Error Codes ========== */

#define ATPD_SPLICE_OK            0
#define ATPD_SPLICE_EOF          -1
#define ATPD_SPLICE_EAGAIN       -2
#define ATPD_SPLICE_NOTSUP       -3
#define ATPD_SPLICE_ERROR        -4
#define ATPD_SPLICE_VPN_NOT_READY -5

/* ========== Session States ========== */

typedef enum {
    ATPD_SESSION_IDLE = 0,
    ATPD_SESSION_ACTIVE,
    ATPD_SESSION_PIPE_DIRTY,
    ATPD_SESSION_DRAINING,
    ATPD_SESSION_CLOSING,
    ATPD_SESSION_DESTROY_PENDING,
    ATPD_SESSION_DESTROYED
} atpd_session_state_t;

/* Forward declaration */
typedef struct atpd_session atpd_session_t;

/* ========== GC Node ========== */

struct session_gc_node {
    atpd_session_t *session;
    struct session_gc_node *next;
};

/* ========== Main Session Structure ========== */

struct atpd_session {
    /* === Identity === */
    uint64_t session_id;
    
    /* === File Descriptors === */
    int fd_in;
    int fd_out;
    int pipe_fds[2];
    
    /* === Atomic State === */
    atomic_int state;
    atomic_bool destroy_started;
    atomic_bool gc_enqueued;
    atomic_bool emergency_drained;
    
    /* === Reference Counting === */
    atomic_uint ref_count;
    
    /* === Pipe Management === */
    atomic_size_t pipe_pending;
    
    /* === Statistics === */
    atomic_ullong bytes_in;
    atomic_ullong bytes_out;
    uint64_t created_at;
    atomic_ullong last_active_at;
    
    /* === Reactor === */
    reactor_t *reactor;
    
    /* === GC === */
    struct session_gc_node gc_node;
    
    /* === Linked List === */
    struct atpd_session *next;
    struct atpd_session *prev;
    bool registry_registered;
};

/* ========== Public API ========== */

atpd_session_t* atpd_session_create(reactor_t *r, int fd_in, int fd_out);
void atpd_session_get(atpd_session_t *s);
void atpd_session_put(atpd_session_t *s);
void atpd_session_mark_closing(atpd_session_t *s);
void atpd_session_destroy(atpd_session_t *s);

ssize_t atpd_session_splice_pump(atpd_session_t *s, size_t max_len);
int atpd_session_drain_pipe(atpd_session_t *s);
int atpd_session_register(reactor_t *r, atpd_session_t *s);

int atpd_session_is_vpn_ready(void);

/* GC */
void atpd_session_gc_enqueue(atpd_session_t *s);
void atpd_session_gc_process(reactor_t *r);

/* Emergency */
void atpd_session_emergency_drain_all(void);
size_t atpd_session_active_count(void);

#endif
