/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 * 
 * Unified Reactor Scheduler Implementation
 */

#include "reactor.h"
#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <signal.h>

/* ========== Internal Structures ========== */

typedef struct reactor_handler_s {
    int fd;
    uint32_t events;
    reactor_io_cb callback;
    void *userdata;
    uint8_t active;
    uint8_t pending_delete;
} reactor_handler_t;

typedef struct reactor_timer_internal_s {
    uint64_t expires_ms;
    uint64_t interval_ms;
    reactor_timer_cb callback;
    void *userdata;
    struct reactor_timer_internal_s *next;
    uint8_t active;
    uint8_t pending_delete;
} reactor_timer_internal_t;

typedef struct {
    reactor_handler_t *handlers[REACTOR_MAX_FD];
    reactor_timer_internal_t *timer_wheel[REACTOR_TIMER_WHEEL_SIZE];
    uint64_t current_time_ms;
    size_t timer_count;
    reactor_signal_cb signal_cb;
    reactor_idle_cb idle_cb;
    reactor_error_cb error_cb;
    void *userdata;
    reactor_stats_t stats;
    sigset_t sigmask;
} reactor_private_t;

/* ========== Internal Helpers ========== */

static uint64_t get_monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void timer_wheel_insert(reactor_private_t *priv, reactor_timer_internal_t *timer) {
    size_t slot = timer->expires_ms % REACTOR_TIMER_WHEEL_SIZE;
    timer->next = priv->timer_wheel[slot];
    priv->timer_wheel[slot] = timer;
    priv->timer_count++;
}

static void timer_wheel_remove(reactor_private_t *priv, reactor_timer_internal_t *timer) {
    size_t slot = timer->expires_ms % REACTOR_TIMER_WHEEL_SIZE;
    reactor_timer_internal_t **pp = &priv->timer_wheel[slot];
    
    while (*pp) {
        if (*pp == timer) {
            *pp = timer->next;
            priv->timer_count--;
            return;
        }
        pp = &(*pp)->next;
    }
}

static void process_expired_timers(reactor_t *r, reactor_private_t *priv) {
    size_t slot = priv->current_time_ms % REACTOR_TIMER_WHEEL_SIZE;
    reactor_timer_internal_t *timer = priv->timer_wheel[slot];
    reactor_timer_internal_t *prev = NULL;
    
    while (timer) {
        reactor_timer_internal_t *next = timer->next;
        
        if (timer->active && !timer->pending_delete &&
            timer->expires_ms <= priv->current_time_ms) {
            
            if (prev) {
                prev->next = next;
            } else {
                priv->timer_wheel[slot] = next;
            }
            priv->timer_count--;
            
            if (timer->callback) {
                priv->stats.timers_fired++;
                reactor_timer_t pub_timer = {
                    .expires_ms = timer->expires_ms,
                    .interval_ms = timer->interval_ms,
                    .callback = timer->callback,
                    .userdata = timer->userdata,
                    .active = 1
                };
                timer->callback(r, &pub_timer, timer->userdata);
            }
            
            if (timer->interval_ms > 0 && timer->active && !timer->pending_delete) {
                timer->expires_ms = priv->current_time_ms + timer->interval_ms;
                timer_wheel_insert(priv, timer);
            } else {
                free(timer);
            }
            
            timer = next;
        } else {
            prev = timer;
            timer = next;
        }
    }
}

static int reactor_signal_init(reactor_t *r, reactor_private_t *priv) {
    sigemptyset(&priv->sigmask);
    
    r->signal_fd = signalfd(-1, &priv->sigmask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (r->signal_fd < 0) {
        LOG_ERROR("signalfd failed: %s", strerror(errno));
        return -1;
    }
    
    return 0;
}

static void reactor_signal_handle(reactor_t *r, reactor_private_t *priv) {
    struct signalfd_siginfo si;
    ssize_t n = read(r->signal_fd, &si, sizeof(si));
    
    if (n != sizeof(si)) return;
    
    priv->stats.signals_received++;
    
    switch (si.ssi_signo) {
        case SIGINT:
        case SIGTERM:
            LOG_INFO("Received termination signal");
            r->running = 0;
            break;
        case SIGHUP:
            LOG_INFO("Received reload signal");
            break;
        default:
            break;
    }
    
    if (priv->signal_cb) {
        priv->signal_cb(r, si.ssi_signo, priv->userdata);
    }
}

/* ========== Public API Implementation ========== */

reactor_t* reactor_create(void) {
    reactor_t *r = calloc(1, sizeof(reactor_t));
    reactor_private_t *priv = calloc(1, sizeof(reactor_private_t));
    
    if (!r || !priv) {
        free(r);
        free(priv);
        return NULL;
    }
    
    r->private_data = priv;
    
    r->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (r->epoll_fd < 0) {
        free(priv);
        free(r);
        return NULL;
    }
    
    r->event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (r->event_fd < 0) {
        close(r->epoll_fd);
        free(priv);
        free(r);
        return NULL;
    }
    
    if (reactor_signal_init(r, priv) < 0) {
        close(r->event_fd);
        close(r->epoll_fd);
        free(priv);
        free(r);
        return NULL;
    }
    
    reactor_add_fd(r, r->signal_fd, REACTOR_EVENT_READ, NULL, r);
    reactor_add_fd(r, r->event_fd, REACTOR_EVENT_READ, NULL, r);
    
    priv->current_time_ms = get_monotonic_ms();
    
    LOG_INFO("Reactor created: epoll_fd=%d", r->epoll_fd);
    return r;
}

void reactor_destroy(reactor_t *r) {
    if (!r) return;
    
    reactor_private_t *priv = r->private_data;
    reactor_stop(r);
    
    for (size_t i = 0; i < REACTOR_TIMER_WHEEL_SIZE; i++) {
        reactor_timer_internal_t *timer = priv->timer_wheel[i];
        while (timer) {
            reactor_timer_internal_t *next = timer->next;
            free(timer);
            timer = next;
        }
    }
    
    for (int i = 0; i < REACTOR_MAX_FD; i++) {
        if (priv->handlers[i]) {
            free(priv->handlers[i]);
        }
    }
    
    close(r->signal_fd);
    close(r->event_fd);
    close(r->epoll_fd);
    
    free(priv);
    free(r);
    
    LOG_INFO("Reactor destroyed");
}

int reactor_run(reactor_t *r) {
    if (!r) return -1;
    
    reactor_private_t *priv = r->private_data;
    struct epoll_event events[REACTOR_MAX_EVENTS];
    
    r->running = 1;
    r->in_loop = 1;
    
    LOG_INFO("Reactor starting main loop");
    
    while (r->running) {
        priv->stats.loop_count++;
        priv->current_time_ms = get_monotonic_ms();
        
        process_expired_timers(r, priv);
        
        int timeout_ms = 1000;
        int nfds = epoll_wait(r->epoll_fd, events, REACTOR_MAX_EVENTS, timeout_ms);
        
        if (nfds < 0) {
            if (errno == EINTR) continue;
            if (priv->error_cb) {
                priv->error_cb(r, errno, strerror(errno), priv->userdata);
            }
            continue;
        }
        
        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;
            uint32_t ev = events[i].events;
            
            priv->stats.events_processed++;
            
            if (fd == r->signal_fd) {
                reactor_signal_handle(r, priv);
                continue;
            }
            
            if (fd == r->event_fd) {
                uint64_t val;
                read(r->event_fd, &val, sizeof(val));
                continue;
            }
            
            if (fd >= 0 && fd < REACTOR_MAX_FD) {
                reactor_handler_t *h = priv->handlers[fd];
                if (h && h->active && !h->pending_delete && h->callback) {
                    h->callback(r, fd, ev, h->userdata);
                }
            }
        }
        
        if (nfds == 0 && priv->idle_cb) {
            priv->stats.idle_calls++;
            priv->idle_cb(r, priv->userdata);
        }
        
        for (int fd = 0; fd < REACTOR_MAX_FD; fd++) {
            reactor_handler_t *h = priv->handlers[fd];
            if (h && h->pending_delete) {
                epoll_ctl(r->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                free(h);
                priv->handlers[fd] = NULL;
            }
        }
    }
    
    r->in_loop = 0;
    LOG_INFO("Reactor main loop exited");
    return 0;
}

void reactor_stop(reactor_t *r) {
    if (!r) return;
    r->running = 0;
    reactor_wakeup(r);
}

int reactor_add_fd(reactor_t *r, int fd, uint32_t events, reactor_io_cb cb, void *userdata) {
    if (!r || fd < 0 || fd >= REACTOR_MAX_FD) return -1;
    
    reactor_private_t *priv = r->private_data;
    
    reactor_handler_t *h = malloc(sizeof(reactor_handler_t));
    if (!h) return -1;
    
    h->fd = fd;
    h->events = events;
    h->callback = cb;
    h->userdata = userdata;
    h->active = 1;
    h->pending_delete = 0;
    
    struct epoll_event ev = {
        .events = events,
        .data = { .fd = fd }
    };
    
    int op = priv->handlers[fd] ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    
    if (epoll_ctl(r->epoll_fd, op, fd, &ev) < 0) {
        free(h);
        return -1;
    }
    
    if (priv->handlers[fd]) {
        free(priv->handlers[fd]);
    }
    priv->handlers[fd] = h;
    priv->stats.active_handlers++;
    
    return 0;
}

int reactor_modify_fd(reactor_t *r, int fd, uint32_t events) {
    if (!r || fd < 0 || fd >= REACTOR_MAX_FD) return -1;
    
    reactor_private_t *priv = r->private_data;
    reactor_handler_t *h = priv->handlers[fd];
    if (!h) return -1;
    
    h->events = events;
    
    struct epoll_event ev = {
        .events = events,
        .data = { .fd = fd }
    };
    
    return epoll_ctl(r->epoll_fd, EPOLL_CTL_MOD, fd, &ev);
}

int reactor_remove_fd(reactor_t *r, int fd) {
    if (!r || fd < 0 || fd >= REACTOR_MAX_FD) return -1;
    
    reactor_private_t *priv = r->private_data;
    reactor_handler_t *h = priv->handlers[fd];
    if (!h) return 0;
    
    h->pending_delete = 1;
    h->active = 0;
    priv->stats.active_handlers--;
    
    reactor_wakeup(r);
    return 0;
}

reactor_timer_t* reactor_add_timer(reactor_t *r, uint64_t timeout_ms, uint64_t interval_ms,
                                   reactor_timer_cb cb, void *userdata) {
    if (!r) return NULL;
    
    reactor_private_t *priv = r->private_data;
    
    reactor_timer_internal_t *timer = calloc(1, sizeof(reactor_timer_internal_t));
    if (!timer) return NULL;
    
    timer->expires_ms = priv->current_time_ms + timeout_ms;
    timer->interval_ms = interval_ms;
    timer->callback = cb;
    timer->userdata = userdata;
    timer->active = 1;
    
    timer_wheel_insert(priv, timer);
    priv->stats.active_timers++;
    
    reactor_timer_t *pub_timer = malloc(sizeof(reactor_timer_t));
    if (pub_timer) {
        pub_timer->expires_ms = timer->expires_ms;
        pub_timer->interval_ms = timer->interval_ms;
        pub_timer->callback = cb;
        pub_timer->userdata = userdata;
        pub_timer->internal = timer;
        pub_timer->active = 1;
    }
    
    return pub_timer;
}

int reactor_cancel_timer(reactor_t *r, reactor_timer_t *timer) {
    if (!r || !timer || !timer->internal) return -1;
    
    reactor_private_t *priv = r->private_data;
    reactor_timer_internal_t *internal = timer->internal;
    
    internal->pending_delete = 1;
    internal->active = 0;
    timer_wheel_remove(priv, internal);
    priv->stats.active_timers--;
    
    free(internal);
    free(timer);
    
    return 0;
}

void reactor_update_time(reactor_t *r) {
    if (r) {
        reactor_private_t *priv = r->private_data;
        priv->current_time_ms = get_monotonic_ms();
    }
}

uint64_t reactor_now_ms(void) {
    return get_monotonic_ms();
}

int reactor_set_signal_cb(reactor_t *r, reactor_signal_cb cb) {
    if (!r) return -1;
    reactor_private_t *priv = r->private_data;
    priv->signal_cb = cb;
    return 0;
}

int reactor_watch_signal(reactor_t *r, int signo) {
    if (!r || signo <= 0) return -1;
    
    reactor_private_t *priv = r->private_data;
    sigaddset(&priv->sigmask, signo);
    
    sigprocmask(SIG_BLOCK, &priv->sigmask, NULL);
    
    close(r->signal_fd);
    r->signal_fd = signalfd(-1, &priv->sigmask, SFD_NONBLOCK | SFD_CLOEXEC);
    
    return (r->signal_fd >= 0) ? 0 : -1;
}

void reactor_set_idle_cb(reactor_t *r, reactor_idle_cb cb) {
    if (r) {
        reactor_private_t *priv = r->private_data;
        priv->idle_cb = cb;
    }
}

void reactor_set_error_cb(reactor_t *r, reactor_error_cb cb) {
    if (r) {
        reactor_private_t *priv = r->private_data;
        priv->error_cb = cb;
    }
}

void reactor_wakeup(reactor_t *r) {
    if (r && r->event_fd >= 0) {
        uint64_t val = 1;
        write(r->event_fd, &val, sizeof(val));
    }
}

const reactor_stats_t* reactor_get_stats(reactor_t *r) {
    if (!r) return NULL;
    
    reactor_private_t *priv = r->private_data;
    priv->stats.active_handlers = 0;
    for (int i = 0; i < REACTOR_MAX_FD; i++) {
        if (priv->handlers[i] && priv->handlers[i]->active) {
            priv->stats.active_handlers++;
        }
    }
    priv->stats.active_timers = priv->timer_count;
    
    return &priv->stats;
}
