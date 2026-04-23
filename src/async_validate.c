/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * Async Config Validation - Non-blocking fork/pipe/epoll validation
 */

#include "async_validate.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/timerfd.h>

static void validate_io_cb(reactor_t *r, int fd, uint32_t events, void *userdata);
static void validate_timeout_cb(reactor_t *r, reactor_timer_t *timer, void *userdata);
static void validate_cleanup(async_validate_ctx_t *ctx, int result, const char *output);

/* ========== Public API ========== */

int async_validate_config(async_validate_ctx_t *ctx, reactor_t *r,
                          const char *bin_path, const char *work_dir,
                          validate_callback_t callback, void *userdata) {
    if (!ctx || !r || !bin_path || !callback) return -1;
    
    memset(ctx, 0, sizeof(async_validate_ctx_t));
    ctx->reactor = r;
    ctx->callback = callback;
    ctx->userdata = userdata;
    
    int pipe_fds[2];
    if (pipe(pipe_fds) < 0) {
        LOG_ERROR("AsyncValidate: pipe failed: %s", strerror(errno));
        return -1;
    }
    
    /* Set read end non-blocking */
    int flags = fcntl(pipe_fds[0], F_GETFL);
    fcntl(pipe_fds[0], F_SETFL, flags | O_NONBLOCK);
    
    pid_t pid = fork();
    if (pid < 0) {
        LOG_ERROR("AsyncValidate: fork failed: %s", strerror(errno));
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return -1;
    }
    
    if (pid == 0) {
        /* Child process */
        close(pipe_fds[0]);
        dup2(pipe_fds[1], STDOUT_FILENO);
        dup2(pipe_fds[1], STDERR_FILENO);
        close(pipe_fds[1]);
        
        int null_fd = open("/dev/null", O_RDWR);
        if (null_fd >= 0) {
            dup2(null_fd, STDIN_FILENO);
            close(null_fd);
        }
        
        execl(bin_path, bin_path, "check", "-D", work_dir, NULL);
        _exit(127);
    }
    
    /* Parent process */
    close(pipe_fds[1]);
    ctx->child_pid = pid;
    ctx->pipe_fd = pipe_fds[0];
    
    /* Create timeout timer (5 seconds) */
    struct itimerspec its = {
        .it_value = { .tv_sec = 5, .tv_nsec = 0 },
        .it_interval = { .tv_sec = 0, .tv_nsec = 0 }
    };
    ctx->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    timerfd_settime(ctx->timer_fd, 0, &its, NULL);
    
    /* Register with reactor */
    reactor_add_fd(r, ctx->pipe_fd, REACTOR_EVENT_READ, validate_io_cb, ctx);
    reactor_add_fd(r, ctx->timer_fd, REACTOR_EVENT_READ, validate_timeout_cb, ctx);
    
    LOG_DEBUG("AsyncValidate: started (PID: %d)", pid);
    return 0;
}

void async_validate_cleanup(async_validate_ctx_t *ctx) {
    if (!ctx) return;
    validate_cleanup(ctx, -1, "cleanup");
}

/* ========== Internal Callbacks ========== */

static void validate_io_cb(reactor_t *r, int fd, uint32_t events, void *userdata) {
    async_validate_ctx_t *ctx = userdata;
    (void)r;
    (void)events;
    
    if (ctx->completed) return;
    
    char buf[1024];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    
    if (n > 0) {
        if (ctx->output_len + n < sizeof(ctx->output) - 1) {
            memcpy(ctx->output + ctx->output_len, buf, n);
            ctx->output_len += n;
            ctx->output[ctx->output_len] = '\0';
        }
    } else if (n == 0) {
        /* EOF - child finished */
        int status;
        waitpid(ctx->child_pid, &status, WNOHANG);
        
        int result = (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 1 : 0;
        LOG_DEBUG("AsyncValidate: completed, result=%d", result);
        validate_cleanup(ctx, result, ctx->output);
    } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
        LOG_ERROR("AsyncValidate: read error: %s", strerror(errno));
        validate_cleanup(ctx, 0, "read error");
    }
}

static void validate_timeout_cb(reactor_t *r, int fd, uint32_t events, void *userdata) {
    async_validate_ctx_t *ctx = userdata;
    (void)r;
    (void)fd;
    (void)events;
    
    if (ctx->completed) return;
    
    uint64_t expirations;
    read(fd, &expirations, sizeof(expirations));
    
    LOG_WARN("AsyncValidate: timeout, killing child");
    kill(ctx->child_pid, SIGKILL);
    waitpid(ctx->child_pid, NULL, 0);
    
    validate_cleanup(ctx, 0, "timeout");
}

static void validate_cleanup(async_validate_ctx_t *ctx, int result, const char *output) {
    if (ctx->completed) return;
    ctx->completed = 1;
    
    reactor_remove_fd(ctx->reactor, ctx->pipe_fd);
    reactor_remove_fd(ctx->reactor, ctx->timer_fd);
    
    close(ctx->pipe_fd);
    close(ctx->timer_fd);
    
    if (ctx->callback) {
        ctx->callback(result, output, ctx->userdata);
    }
}
