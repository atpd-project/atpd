/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2025 ATP Project
 *
 * Epoll-based event loop implementation
 */

#include "atp.h"
#include "logger.h"
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

extern volatile sig_atomic_t g_reload;
extern volatile sig_atomic_t g_show_status;

#define MAX_EPOLL_EVENTS 16

static int g_epoll_fd = -1;
static int g_sig_fd = -1;
static volatile sig_atomic_t g_epoll_running = 0;

typedef struct {
    int fd;
    void (*callback)(int fd, void *data);
    void *data;
} epoll_callback_t;

static epoll_callback_t *g_callbacks = NULL;
static int g_callback_count = 0;
static int g_callback_capacity = 0;

int epoll_init(void) {
    sigset_t mask;
    
    g_epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (g_epoll_fd < 0) {
        LOG_ERROR("epoll_create1 failed: %s", strerror(errno));
        return -1;
    }
    
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGHUP);
    sigaddset(&mask, SIGUSR1);
    
    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
        LOG_ERROR("sigprocmask failed: %s", strerror(errno));
        close(g_epoll_fd);
        g_epoll_fd = -1;
        return -1;
    }
    
    g_sig_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (g_sig_fd < 0) {
        LOG_ERROR("signalfd failed: %s", strerror(errno));
        close(g_epoll_fd);
        g_epoll_fd = -1;
        return -1;
    }
    
    g_callback_capacity = 8;
    g_callbacks = calloc(g_callback_capacity, sizeof(epoll_callback_t));
    if (!g_callbacks) {
        LOG_ERROR("calloc failed");
        close(g_sig_fd);
        close(g_epoll_fd);
        g_sig_fd = -1;
        g_epoll_fd = -1;
        return -1;
    }
    
    g_epoll_running = 0;
    
    LOG_INFO("Epoll initialized, epoll_fd=%d, sig_fd=%d", g_epoll_fd, g_sig_fd);
    return 0;
}

void epoll_cleanup(void) {
    if (g_callbacks) {
        free(g_callbacks);
        g_callbacks = NULL;
        g_callback_count = 0;
        g_callback_capacity = 0;
    }
    if (g_sig_fd >= 0) {
        close(g_sig_fd);
        g_sig_fd = -1;
    }
    if (g_epoll_fd >= 0) {
        close(g_epoll_fd);
        g_epoll_fd = -1;
    }
    LOG_INFO("Epoll cleaned up");
}

int epoll_add_fd(int fd, void (*callback)(int, void*), void *data) {
    struct epoll_event ev;
    
    if (g_epoll_fd < 0 || fd < 0 || !callback) {
        return -1;
    }
    
    ev.events = EPOLLIN;
    ev.data.ptr = (void *)(intptr_t)g_callback_count;
    
    if (epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        LOG_ERROR("epoll_ctl ADD fd=%d: %s", fd, strerror(errno));
        return -1;
    }
    
    if (g_callback_count >= g_callback_capacity) {
        int new_cap = g_callback_capacity * 2;
        epoll_callback_t *new_cb = realloc(g_callbacks, new_cap * sizeof(epoll_callback_t));
        if (!new_cb) {
            epoll_ctl(g_epoll_fd, EPOLL_CTL_DEL, fd, NULL);
            return -1;
        }
        g_callbacks = new_cb;
        g_callback_capacity = new_cap;
    }
    
    g_callbacks[g_callback_count].fd = fd;
    g_callbacks[g_callback_count].callback = callback;
    g_callbacks[g_callback_count].data = data;
    g_callback_count++;
    
    LOG_DEBUG("Added fd=%d to epoll", fd);
    return 0;
}

int epoll_remove_fd(int fd) {
    if (g_epoll_fd < 0 || fd < 0) {
        return -1;
    }
    
    if (epoll_ctl(g_epoll_fd, EPOLL_CTL_DEL, fd, NULL) < 0) {
        LOG_ERROR("epoll_ctl DEL fd=%d: %s", fd, strerror(errno));
        return -1;
    }
    
    for (int i = 0; i < g_callback_count; i++) {
        if (g_callbacks[i].fd == fd) {
            memmove(&g_callbacks[i], &g_callbacks[i + 1],
                    (g_callback_count - i - 1) * sizeof(epoll_callback_t));
            g_callback_count--;
            break;
        }
    }
    
    LOG_DEBUG("Removed fd=%d from epoll", fd);
    return 0;
}

static void epoll_stop(void);
static void handle_signal_fd(int fd, void *data) {
    struct signalfd_siginfo siginfo;
    ssize_t len;
    
    (void)data;
    
    len = read(fd, &siginfo, sizeof(siginfo));
    if (len != sizeof(siginfo)) {
        if (len < 0 && errno != EAGAIN) {
            LOG_ERROR("read signalfd: %s", strerror(errno));
        }
        return;
    }
    
    switch (siginfo.ssi_signo) {
        case SIGTERM:
        case SIGINT:
            LOG_INFO("Received signal %d, stopping", siginfo.ssi_signo);
            epoll_stop();
            break;
        case SIGHUP:
            LOG_INFO("Received SIGHUP, reloading");
            extern volatile sig_atomic_t g_reload;
            g_reload = 1;
            break;
        case SIGUSR1:
            LOG_INFO("Received SIGUSR1, showing status");
            extern volatile sig_atomic_t g_show_status;
            g_show_status = 1;
            break;
        default:
            break;
    }
}

void epoll_run(void) {
    struct epoll_event events[MAX_EPOLL_EVENTS];
    int nfds;
    
    epoll_add_fd(g_sig_fd, handle_signal_fd, NULL);
    
    g_epoll_running = 1;
    LOG_INFO("Epoll event loop started");
    
    while (g_epoll_running) {
        nfds = epoll_wait(g_epoll_fd, events, MAX_EPOLL_EVENTS, -1);
        if (nfds < 0) {
            if (errno == EINTR) {
                continue;
            }
            LOG_ERROR("epoll_wait: %s", strerror(errno));
            break;
        }
        
        for (int i = 0; i < nfds; i++) {
            int idx = (int)(intptr_t)events[i].data.ptr;
            if (idx >= 0 && idx < g_callback_count) {
                g_callbacks[idx].callback(g_callbacks[idx].fd,
                                          g_callbacks[idx].data);
            }
        }
    }
    
    LOG_INFO("Epoll event loop stopped");
}

int epoll_run_once(int timeout_ms) {
    struct epoll_event events[MAX_EPOLL_EVENTS];
    int nfds;

    if (!g_epoll_running) {
        epoll_add_fd(g_sig_fd, handle_signal_fd, NULL);
        g_epoll_running = 1;
    }

    nfds = epoll_wait(g_epoll_fd, events, MAX_EPOLL_EVENTS, timeout_ms);
    if (nfds < 0) {
        if (errno == EINTR) {
            return 0;
        }
        LOG_ERROR("epoll_wait: %s", strerror(errno));
        return -1;
    }

    for (int i = 0; i < nfds; i++) {
        int idx = (int)(intptr_t)events[i].data.ptr;
        if (idx >= 0 && idx < g_callback_count) {
            g_callbacks[idx].callback(g_callbacks[idx].fd,
                                      g_callbacks[idx].data);
        }
    }

    return nfds;
}

void epoll_stop(void) {
    g_epoll_running = 0;
}
