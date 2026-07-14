#include "session.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <pthread.h>

/*
 * ============================================================================
 * Reactor Ownership Contract
 * ============================================================================
 *
 * This module manages session lifecycle with explicit reference counting.
 * The reactor integration follows a strict ownership model:
 *
 * 1. Reference Counting Rules:
 *
 *    atpd_session_create()      -> +1 (initial reference)
 *    reactor_add_fd_ex(fd_in)   -> +1 (reactor holds for IN callback)
 *    reactor_add_fd_ex(fd_out)  -> +1 (reactor holds for OUT callback)
 *    atpd_session_gc_enqueue()  -> +1 (GC queue holds during cleanup)
 *
 *    session_free_cb(fd_in)     -> -1 (reactor releases IN reference)
 *    session_free_cb(fd_out)    -> -1 (reactor releases OUT reference)
 *    atpd_session_gc_process()  -> -1 (GC releases its reference)
 *
 * 2. Critical Invariants:
 *
 *    - reactor_remove_fd() MUST trigger free_cb() exactly once per fd
 *    - free_cb() is the ONLY place that releases reactor-held references
 *    - mark_closing() does NOT release reactor references
 *    - GC enqueue takes its own reference, process releases it
 *
 * 3. Destroy Path:
 *
 *    mark_closing()
 *      -> reactor_remove_fd(fd_in)  -> free_cb(fd_in)  -> put()
 *      -> reactor_remove_fd(fd_out) -> free_cb(fd_out) -> put()
 *      -> gc_enqueue()              -> get()
 *      -> gc_process()              -> put() -> ref=0 -> destroy()
 *
 * ============================================================================
 */

/*
 * ============================================================================
 * Session State Machine
 * ============================================================================
 *
 *   ┌─────────────────────────────────────────────────────────────┐
 *   │                                                             │
 *   │                     IDLE                                    │
 *   │                      │                                      │
 *   │                      │ atpd_session_register()             │
 *   │                      ▼                                      │
 *   │                    ACTIVE                                   │
 *   │                      │                                      │
 *   │                      │ splice returns EAGAIN               │
 *   │                      ▼                                      │
 *   │               ┌────────────┐     ┌─────────────┐          │
 *   │               │ PIPE_DIRTY │────▶│  DRAINING   │          │
 *   │               └────────────┘     └─────────────┘          │
 *   │                      │                      │              │
 *   │                      │ drain complete       │              │
 *   │                      └──────────────────────┘              │
 *   │                              │                             │
 *   │                              │ error / shutdown            │
 *   │                              ▼                             │
 *   │                         ┌──────────┐                      │
 *   │                         │ CLOSING  │                      │
 *   │                         └──────────┘                      │
 *   │                              │                             │
 *   │                              │ atpd_session_gc_enqueue()  │
 *   │                              ▼                             │
 *   │                    ┌──────────────────┐                   │
 *   │                    │ DESTROY_PENDING  │                   │
 *   │                    └──────────────────┘                   │
 *   │                              │                             │
 *   │                              │ gc_process()               │
 *   │                              ▼                             │
 *   │                    ┌──────────────────┐                   │
 *   │                    │   DESTROYED      │                   │
 *   │                    └──────────────────┘                   │
 *   │                                                             │
 *   └─────────────────────────────────────────────────────────────┘
 *
 * Valid Transitions:
 *   IDLE         -> ACTIVE       (register)
 *   IDLE         -> CLOSING      (register fail)
 *   ACTIVE       -> PIPE_DIRTY   (write blocked)
 *   PIPE_DIRTY   -> DRAINING     (drain started)
 *   DRAINING     -> ACTIVE       (drain complete)
 *   DRAINING     -> PIPE_DIRTY   (drain blocked again)
 *   ACTIVE       -> CLOSING      (error/shutdown)
 *   PIPE_DIRTY   -> CLOSING      (error/shutdown)
 *   DRAINING     -> CLOSING      (error/shutdown)
 *   CLOSING      -> DESTROY_PENDING (gc enqueue)
 *   DESTROY_PENDING -> DESTROYED    (gc process)
 *
 * Invalid Transitions (rejected by CAS):
 *   DESTROYED    -> any
 *   DESTROY_PENDING -> any except DESTROYED
 *
 * ============================================================================
 */

/* ========== Forward Declarations ========== */

static void session_in_cb(reactor_t *r, int fd, uint32_t events, void *userdata);
static void session_out_cb(reactor_t *r, int fd, uint32_t events, void *userdata);
static void session_free_cb(void *userdata);
static void atpd_session_destroy_internal(atpd_session_t *s);
static inline void safe_close(int *fd);
static void emergency_drain(atpd_session_t *s);

/* ========== Global GC Queue with Mutex ========== */

static pthread_mutex_t g_gc_lock = PTHREAD_MUTEX_INITIALIZER;

static struct {
    struct session_gc_node *head;
    struct session_gc_node *tail;
} g_gc_queue = { NULL, NULL };

/* ========== Session Lifecycle ========== */

atpd_session_t* atpd_session_create(reactor_t *r, int fd_in, int fd_out) {
    static atomic_uint_fast64_t next_session_id = 1;
    
    atpd_session_t *s = calloc(1, sizeof(atpd_session_t));
    if (!s) return NULL;
    
    s->session_id = atomic_fetch_add_explicit(&next_session_id, 1, memory_order_relaxed);
    s->fd_in = fd_in;
    s->fd_out = fd_out;
    s->reactor = r;
    s->created_at = reactor_now_ms();
    
    atomic_init(&s->state, ATPD_SESSION_IDLE);
    atomic_init(&s->destroy_started, false);
    atomic_init(&s->gc_enqueued, false);
    atomic_init(&s->emergency_drained, false);
    atomic_init(&s->ref_count, 1);
    atomic_init(&s->pipe_pending, 0);
    atomic_init(&s->bytes_in, 0);
    atomic_init(&s->bytes_out, 0);
    atomic_init(&s->last_active_at, s->created_at);
    
    if (pipe2(s->pipe_fds, O_NONBLOCK | O_CLOEXEC) < 0) {
        LOG_ERROR("SESSION[%lu]: pipe2 failed: %s", s->session_id, strerror(errno));
        free(s);
        return NULL;
    }
    
    fcntl(s->pipe_fds[0], F_SETPIPE_SZ, ATPD_SESSION_PIPE_SIZE);
    fcntl(s->pipe_fds[1], F_SETPIPE_SZ, ATPD_SESSION_PIPE_SIZE);
    
    atpd_session_register_to_ctx(s);
    
    LOG_DEBUG("SESSION[%lu]: created fd_in=%d fd_out=%d ref=1", 
              s->session_id, fd_in, fd_out);
    return s;
}

void atpd_session_get(atpd_session_t *s) {
    if (!s) return;
    unsigned int old = atomic_fetch_add_explicit(&s->ref_count, 1, memory_order_relaxed);
    LOG_DEBUG("SESSION[%lu]: get ref=%u", s->session_id, old + 1);
}

void atpd_session_put(atpd_session_t *s) {
    if (!s) return;
    unsigned int old = atomic_fetch_sub_explicit(&s->ref_count, 1, memory_order_acq_rel);
    LOG_DEBUG("SESSION[%lu]: put ref=%u", s->session_id, old - 1);
    if (old == 1) {
        /* Last reference dropped */
        /* Acquire memory fence to ensure all previous operations are visible */
        atomic_thread_fence(memory_order_acquire);
        atpd_session_destroy_internal(s);
    }
}

static inline void safe_close(int *fd) {
    if (fd && *fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static void atpd_session_destroy_internal(atpd_session_t *s) {
    if (!s) return;
    
    /* Cache session data before CAS */
    uint64_t sid = s->session_id;
    
    /* Atomic CAS: only one thread can destroy */
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s->destroy_started, &expected, true)) {
        LOG_DEBUG("SESSION[%lu]: destroy already started", sid);
        return;
    }
    
    int fd_in = s->fd_in;
    int fd_out = s->fd_out;
    uint64_t bytes_in = atomic_load(&s->bytes_in);
    uint64_t bytes_out = atomic_load(&s->bytes_out);
    unsigned int refs = atomic_load(&s->ref_count);
    int state = atomic_load(&s->state);
    
    /* Assert: no references should remain */
    if (refs != 0) {
        LOG_WARN("SESSION[%lu]: destroy with non-zero ref=%u - potential leak!", 
                 sid, refs);
    }
    
    LOG_DEBUG("SESSION[%lu]: destroying fd_in=%d fd_out=%d bytes_in=%" PRIu64 " bytes_out=%" PRIu64 " final_ref=%u state=%d",
              sid, fd_in, fd_out, bytes_in, bytes_out, refs, state);
    
    atomic_store(&s->state, ATPD_SESSION_DESTROYED);
    
    atpd_session_unregister_from_ctx(s);
    
    safe_close(&s->pipe_fds[0]);
    safe_close(&s->pipe_fds[1]);
    safe_close(&s->fd_in);
    safe_close(&s->fd_out);
    
    s->reactor = NULL;
    free(s);
    
    LOG_DEBUG("SESSION[%lu]: destroyed", sid);
}

/* ========== Session Registration ========== */

int atpd_session_register(reactor_t *r, atpd_session_t *s) {
    if (!s) return -1;
    
    /* Hold reference for each callback */
    atpd_session_get(s);
    reactor_add_fd_ex(r, s->fd_in, REACTOR_EVENT_READ | REACTOR_EVENT_EDGE,
                      session_in_cb, session_free_cb, s);
    
    atpd_session_get(s);
    reactor_add_fd_ex(r, s->fd_out, REACTOR_EVENT_READ | REACTOR_EVENT_EDGE,
                      session_out_cb, session_free_cb, s);
    
    atomic_store(&s->state, ATPD_SESSION_ACTIVE);
    LOG_DEBUG("SESSION[%lu]: registered, ref=%u", s->session_id, atomic_load(&s->ref_count));
    return 0;
}

/* ========== VPN State ========== */

int atpd_session_is_vpn_ready(void) {
    return atomic_load_explicit(&g_atpd_ctx.vpn_state, memory_order_acquire) == VPN_STATE_READY;
}

/* ========== Pipe Drain ========== */

int atpd_session_drain_pipe(atpd_session_t *s) {
    if (!s) return -1;
    
    size_t pending = atomic_load(&s->pipe_pending);
    if (pending == 0) return 0;
    
    int state = atomic_load(&s->state);
    if (state != ATPD_SESSION_PIPE_DIRTY) {
        return 0;
    }
    
    atomic_store(&s->state, ATPD_SESSION_DRAINING);
    
    ssize_t sent = 0;
    while (sent < (ssize_t)pending) {
        ssize_t ret = splice(s->pipe_fds[0], NULL, s->fd_out, NULL,
                             pending - sent,
                             SPLICE_F_MOVE | SPLICE_F_NONBLOCK);
        
        if (ret < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                atomic_store(&s->pipe_pending, pending - sent);
                atomic_store(&s->state, ATPD_SESSION_PIPE_DIRTY);
                return 1;  /* Still dirty */
            }
            if (errno == EINTR) continue;
            LOG_ERROR("SESSION[%lu]: drain pipe failed: %s", s->session_id, strerror(errno));
            atomic_store(&s->state, ATPD_SESSION_PIPE_DIRTY);
            return -1;
        }
        if (ret == 0) break;
        sent += ret;
        atomic_fetch_add(&s->bytes_out, (uint64_t)ret);
    }
    
    atomic_store(&s->pipe_pending, 0);
    atomic_store(&s->state, ATPD_SESSION_ACTIVE);
    
    /* Remove WRITE event after drain complete */
    if (s->reactor) {
        reactor_modify_fd(s->reactor, s->fd_out, REACTOR_EVENT_READ | REACTOR_EVENT_EDGE);
    }
    
    LOG_DEBUG("SESSION[%lu]: pipe drained, sent=%zd", s->session_id, sent);
    return 0;
}

/* ========== Splice Pump ========== */

ssize_t atpd_session_splice_pump(atpd_session_t *s, size_t max_len) {
    if (!s) return ATPD_SPLICE_ERROR;
    
    /* Check VPN state */
    if (!atpd_session_is_vpn_ready()) {
        return ATPD_SPLICE_VPN_NOT_READY;
    }
    
    int state = atomic_load(&s->state);
    if (state >= ATPD_SESSION_CLOSING) {
        return ATPD_SPLICE_ERROR;
    }
    
    ssize_t total_moved = 0;
    size_t remaining = max_len;
    uint64_t bytes_in_total = 0;
    uint64_t bytes_out_total = 0;
    
    while (remaining > 0) {
        /* Read: fd_in -> pipe */
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
                    LOG_ERROR("SESSION[%lu]: splice not supported for fd_in", s->session_id);
                    return ATPD_SPLICE_NOTSUP;
                default:
                    LOG_ERROR("SESSION[%lu]: splice read error: %s", 
                              s->session_id, strerror(errno));
                    return ATPD_SPLICE_ERROR;
            }
        }
        
        if (moved == 0) {
            if (total_moved > 0) goto done;
            return ATPD_SPLICE_EOF;
        }
        
        /* bytes_in += moved (data entered pipe) */
        bytes_in_total += (uint64_t)moved;
        
        /* Write: pipe -> fd_out */
        ssize_t sent = 0;
        size_t to_send = (size_t)moved;
        
        while (sent < (ssize_t)to_send) {
            /* Double-check VPN state */
            if (!atpd_session_is_vpn_ready()) {
                LOG_WARN("SESSION[%lu]: VPN became not ready during splice", s->session_id);
                atpd_session_mark_closing(s);
                emergency_drain(s);
                if (total_moved > 0) {
                    atomic_fetch_add(&s->bytes_in, bytes_in_total);
                    atomic_fetch_add(&s->bytes_out, bytes_out_total);
                    return total_moved;
                }
                return ATPD_SPLICE_VPN_NOT_READY;
            }
            
            ssize_t ret = splice(s->pipe_fds[0], NULL, s->fd_out, NULL,
                                 to_send - (size_t)sent,
                                 SPLICE_F_MOVE | SPLICE_F_NONBLOCK);
            
            if (ret < 0) {
                switch (errno) {
                    case EAGAIN:
#if EAGAIN != EWOULDBLOCK
                    case EWOULDBLOCK:
#endif
                        atomic_store(&s->pipe_pending, to_send - (size_t)sent);
                        atomic_store(&s->state, ATPD_SESSION_PIPE_DIRTY);
                        bytes_out_total += (uint64_t)sent;
                        atomic_fetch_add(&s->bytes_in, bytes_in_total);
                        atomic_fetch_add(&s->bytes_out, bytes_out_total);
                        /* Enable WRITE event */
                        if (s->reactor) {
                            reactor_modify_fd(s->reactor, s->fd_out, 
                                              REACTOR_EVENT_READ | REACTOR_EVENT_WRITE | REACTOR_EVENT_EDGE);
                        }
                        LOG_DEBUG("SESSION[%lu]: pipe dirty, pending=%zu", 
                                  s->session_id, atomic_load(&s->pipe_pending));
                        return total_moved + sent;
                    case EINTR:
                        continue;
                    case EPIPE:
                    case ECONNRESET:
                        atomic_store(&s->pipe_pending, to_send - (size_t)sent);
                        atomic_store(&s->state, ATPD_SESSION_PIPE_DIRTY);
                        bytes_out_total += (uint64_t)sent;
                        if (total_moved + sent > 0) {
                            atomic_fetch_add(&s->bytes_in, bytes_in_total);
                            atomic_fetch_add(&s->bytes_out, bytes_out_total);
                            return total_moved + sent;
                        }
                        return ATPD_SPLICE_EOF;
                    default:
                        LOG_ERROR("SESSION[%lu]: splice write error: %s", 
                                  s->session_id, strerror(errno));
                        return ATPD_SPLICE_ERROR;
                }
            }
            
            if (ret == 0) {
                if (total_moved + sent > 0) {
                    bytes_out_total += (uint64_t)sent;
                    atomic_fetch_add(&s->bytes_in, bytes_in_total);
                    atomic_fetch_add(&s->bytes_out, bytes_out_total);
                    return total_moved + sent;
                }
                return ATPD_SPLICE_EOF;
            }
            
            sent += ret;
            bytes_out_total += (uint64_t)ret;
        }
        
        total_moved += sent;
        remaining -= (size_t)sent;
    }
    
done:
    if (total_moved > 0) {
        atomic_fetch_add(&s->bytes_in, bytes_in_total);
        atomic_fetch_add(&s->bytes_out, bytes_out_total);
        atomic_store(&s->last_active_at, reactor_now_ms());
    }
    return total_moved > 0 ? total_moved : ATPD_SPLICE_OK;
}

/* ========== IN Callback ========== */

static void session_in_cb(reactor_t *r, int fd, uint32_t events, void *userdata) {
    (void)r;
    (void)fd;
    atpd_session_t *s = userdata;
    
    if (!s) return;
    
    /* Get reference for this callback */
    atpd_session_get(s);
    
    int state = atomic_load(&s->state);
    if (state >= ATPD_SESSION_CLOSING) {
        atpd_session_put(s);
        return;
    }
    
    if (events & (REACTOR_EVENT_ERROR | REACTOR_EVENT_HANGUP)) {
        LOG_DEBUG("SESSION[%lu]: IN error/hangup", s->session_id);
        atpd_session_mark_closing(s);
        atpd_session_put(s);
        return;
    }
    
    if (state == ATPD_SESSION_PIPE_DIRTY) {
        atpd_session_drain_pipe(s);
        state = atomic_load(&s->state);
        if (state == ATPD_SESSION_PIPE_DIRTY) {
            atpd_session_put(s);
            return;
        }
    }
    
    ssize_t ret = atpd_session_splice_pump(s, ATPD_SESSION_PIPE_SIZE);
    if (ret == ATPD_SPLICE_VPN_NOT_READY) {
        LOG_DEBUG("SESSION[%lu]: VPN not ready, marking closing", s->session_id);
        atpd_session_mark_closing(s);
        atpd_session_put(s);
        return;
    }
    if (ret < 0) {
        LOG_ERROR("SESSION[%lu]: splice pump failed: %zd", s->session_id, ret);
        atpd_session_mark_closing(s);
        atpd_session_put(s);
        return;
    }
    
    atpd_session_put(s);
}

/* ========== OUT Callback ========== */

static void session_out_cb(reactor_t *r, int fd, uint32_t events, void *userdata) {
    (void)r;
    (void)fd;
    atpd_session_t *s = userdata;
    
    if (!s) return;
    
    /* Get reference for this callback */
    atpd_session_get(s);
    
    int state = atomic_load(&s->state);
    if (state >= ATPD_SESSION_CLOSING) {
        atpd_session_put(s);
        return;
    }
    
    if (events & (REACTOR_EVENT_ERROR | REACTOR_EVENT_HANGUP)) {
        LOG_DEBUG("SESSION[%lu]: OUT error/hangup", s->session_id);
        atpd_session_mark_closing(s);
        atpd_session_put(s);
        return;
    }
    
    if (state == ATPD_SESSION_PIPE_DIRTY) {
        int ret = atpd_session_drain_pipe(s);
        if (ret > 0) {
            /* Still dirty, keep WRITE event */
            atpd_session_put(s);
            return;
        }
        if (ret < 0) {
            LOG_ERROR("SESSION[%lu]: drain pipe failed", s->session_id);
            atpd_session_mark_closing(s);
            atpd_session_put(s);
            return;
        }
        /* Drained successfully */
        LOG_DEBUG("SESSION[%lu]: pipe drained via OUT callback", s->session_id);
    }
    
    atpd_session_put(s);
}

/* ========== Free Callback ========== */

static void session_free_cb(void *userdata) {
    atpd_session_t *s = userdata;
    if (!s) return;
    
    LOG_DEBUG("SESSION[%lu]: free_cb called", s->session_id);
    
    /* Release reactor-held reference */
    /* This is the only place that releases the reactor reference */
    atpd_session_put(s);
}

/* ========== Mark Closing ========== */

void atpd_session_mark_closing(atpd_session_t *s) {
    if (!s) return;
    
    /* Loop CAS: allow transition from any state to CLOSING */
    for (;;) {
        int state = atomic_load(&s->state);
        
        /* Already closing or beyond */
        if (state >= ATPD_SESSION_CLOSING) {
            LOG_DEBUG("SESSION[%lu]: already closing or destroyed (state=%d)", 
                      s->session_id, state);
            return;
        }
        
        /* Try to atomically transition to CLOSING */
        if (atomic_compare_exchange_weak(&s->state, &state, ATPD_SESSION_CLOSING)) {
            break;
        }
        /* CAS failed, retry with updated state value */
    }
    
    LOG_DEBUG("SESSION[%lu]: marking closing", s->session_id);
    
    /* Remove from reactor - this will trigger free_cb for each fd */
    if (s->reactor) {
        reactor_remove_fd(s->reactor, s->fd_in);
        reactor_remove_fd(s->reactor, s->fd_out);
    }
    
    /* DO NOT put here - free_cb will release the reactor references */
    /* The reactor holds references that will be released via free_cb */
    
    /* Enqueue for GC - GC takes its own reference */
    atpd_session_gc_enqueue(s);
}

/* ========== GC Queue ========== */

void atpd_session_gc_enqueue(atpd_session_t *s) {
    if (!s) return;
    
    /* Prevent double enqueue */
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s->gc_enqueued, &expected, true)) {
        LOG_DEBUG("SESSION[%lu]: already enqueued", s->session_id);
        return;
    }
    
    /* GC takes a reference */
    atpd_session_get(s);
    
    atomic_store(&s->state, ATPD_SESSION_DESTROY_PENDING);
    s->gc_node.session = s;
    s->gc_node.next = NULL;
    
    pthread_mutex_lock(&g_gc_lock);
    
    if (g_gc_queue.tail == NULL) {
        g_gc_queue.head = &s->gc_node;
        g_gc_queue.tail = &s->gc_node;
    } else {
        g_gc_queue.tail->next = &s->gc_node;
        g_gc_queue.tail = &s->gc_node;
    }
    
    pthread_mutex_unlock(&g_gc_lock);
    
    LOG_DEBUG("SESSION[%lu]: enqueued for GC", s->session_id);
}

void atpd_session_gc_process(reactor_t *r) {
    (void)r;
    
    pthread_mutex_lock(&g_gc_lock);
    
    struct session_gc_node *node = g_gc_queue.head;
    g_gc_queue.head = NULL;
    g_gc_queue.tail = NULL;
    
    pthread_mutex_unlock(&g_gc_lock);
    
    struct session_gc_node *next;
    while (node) {
        next = node->next;
        atpd_session_t *s = node->session;
        
        if (s) {
            /* Release GC reference */
            atpd_session_put(s);
        }
        
        node = next;
    }
    
    LOG_DEBUG("GC: processed all pending sessions");
}

/* ========== Emergency Drain ========== */

static void emergency_drain(atpd_session_t *s) {
    if (!s) return;
    
    /* Prevent double drain */
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s->emergency_drained, &expected, true)) {
        LOG_DEBUG("SESSION[%lu]: already emergency drained", s->session_id);
        return;
    }
    
    LOG_WARN("SESSION[%lu]: emergency drain started", s->session_id);
    
    /* Close pipes - kernel discards data */
    safe_close(&s->pipe_fds[0]);
    safe_close(&s->pipe_fds[1]);
    atomic_store(&s->pipe_pending, 0);
    
    /* Mark for closure if not already */
    int state = atomic_load(&s->state);
    if (state < ATPD_SESSION_CLOSING) {
        atpd_session_mark_closing(s);
    }
    
    LOG_WARN("SESSION[%lu]: emergency drain completed", s->session_id);
}

void atpd_session_emergency_drain_all(void) {
    struct atpd_session_list *node = g_atpd_ctx.sessions;
    struct atpd_session_list *next;
    int drained = 0;
    
    while (node) {
        next = node->next;  /* Safe traversal */
        atpd_session_t *s = node->session;
        if (s) {
            int state = atomic_load(&s->state);
            if (state < ATPD_SESSION_CLOSING) {
                emergency_drain(s);
                drained++;
            }
        }
        node = next;
    }
    
    LOG_WARN("session: emergency drain completed for %d sessions", drained);
}
/* ========== Destroy Wrapper ========== */

void atpd_session_destroy(atpd_session_t *s) {
    if (!s) return;
    atpd_session_mark_closing(s);
}
