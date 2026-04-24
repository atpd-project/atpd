/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 * 
 * Unified Reactor Scheduler - Event-driven I/O multiplexing
 * Compatible with C11, Clang 19 static compilation
 */

#ifndef REACTOR_H
#define REACTOR_H

#include <stdint.h>
#include <stddef.h>
#include <sys/epoll.h>

/* ========== Constants ========== */
#define REACTOR_MAX_EVENTS       1024
#define REACTOR_MAX_FD           65536
#define REACTOR_TIMER_WHEEL_SIZE 256

/* ========== Event Types ========== */
typedef enum {
    REACTOR_EVENT_READ   = EPOLLIN,
    REACTOR_EVENT_WRITE  = EPOLLOUT,
    REACTOR_EVENT_ERROR  = EPOLLERR,
    REACTOR_EVENT_HANGUP = EPOLLHUP,
    REACTOR_EVENT_EDGE   = EPOLLET
} reactor_event_t;

/* ========== Forward Declarations ========== */
typedef struct reactor_s reactor_t;
typedef struct reactor_timer_s reactor_timer_t;

/* ========== Callback Types ========== */
typedef void (*reactor_io_cb)(reactor_t *r, int fd, uint32_t events, void *userdata);
typedef void (*reactor_timer_cb)(reactor_t *r, reactor_timer_t *timer, void *userdata);
typedef void (*reactor_signal_cb)(reactor_t *r, int signo, void *userdata);
typedef void (*reactor_idle_cb)(reactor_t *r, void *userdata);
typedef void (*reactor_error_cb)(reactor_t *r, int error, const char *msg, void *userdata);

/* ========== Reactor Statistics ========== */
typedef struct {
    uint64_t loop_count;
    uint64_t events_processed;
    uint64_t timers_fired;
    uint64_t signals_received;
    uint64_t idle_calls;
    size_t   active_handlers;
    size_t   active_timers;
} reactor_stats_t;

/* ========== Timer Structure ========== */
struct reactor_timer_s {
    uint64_t expires_ms;
    uint64_t interval_ms;
    reactor_timer_cb callback;
    void *userdata;
    void *internal;  /* Private data */
    uint8_t active;
};

/* ========== Reactor Main Structure ========== */
struct reactor_s {
    int epoll_fd;
    int signal_fd;
    int event_fd;
    uint8_t running;
    uint8_t in_loop;
    
    /* Private data follows - opaque to users */
    void *private_data;
};

/* ========== Lifecycle ========== */
reactor_t* reactor_create(void);
void reactor_destroy(reactor_t *r);
int reactor_run(reactor_t *r);
void reactor_stop(reactor_t *r);

/* ========== I/O Handlers ========== */
int reactor_add_fd(reactor_t *r, int fd, uint32_t events, reactor_io_cb cb, void *userdata);
int reactor_modify_fd(reactor_t *r, int fd, uint32_t events);
int reactor_remove_fd(reactor_t *r, int fd);

/* ========== Timers ========== */
reactor_timer_t* reactor_add_timer(reactor_t *r, uint64_t timeout_ms, uint64_t interval_ms,
                                   reactor_timer_cb cb, void *userdata);
int reactor_cancel_timer(reactor_t *r, reactor_timer_t *timer);
void reactor_update_time(reactor_t *r);
uint64_t reactor_now_ms(void);

/* ========== Signals ========== */
int reactor_set_signal_cb(reactor_t *r, reactor_signal_cb cb);
int reactor_watch_signal(reactor_t *r, int signo);

/* ========== Idle & Error ========== */
void reactor_set_idle_cb(reactor_t *r, reactor_idle_cb cb);
void reactor_set_error_cb(reactor_t *r, reactor_error_cb cb);

/* ========== Utilities ========== */
void reactor_wakeup(reactor_t *r);
const reactor_stats_t* reactor_get_stats(reactor_t *r);

#endif /* REACTOR_H */
