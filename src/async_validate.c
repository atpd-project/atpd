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
#include <stdatomic.h>

static void validate_io_cb(reactor_t *r, int fd, uint32_t events, void *userdata);
static void validate_timeout_cb(reactor_t *r, int fd, uint32_t events, void *userdata);
static void validate_cleanup(async_validate_ctx_t *ctx, int result, const char *output);

static pid_t reap_child(async_validate_ctx_t *ctx, int blocking, int *status_out) {
    pid_t result;
    if (!ctx || ctx->child_pid <= 0) return -1;
    if (atomic_load(&ctx->child_reaped)) {
        if (status_out && ctx->child_status_valid) *status_out = ctx->child_status;
        return ctx->child_pid;
    }
    do {
        result = waitpid(ctx->child_pid, status_out, blocking ? 0 : WNOHANG);
    } while (result < 0 && errno == EINTR);
    if (result == ctx->child_pid) {
        if (status_out) {
            ctx->child_status = *status_out;
            ctx->child_status_valid = 1;
        }
        atomic_store(&ctx->child_reaped, 1);
    } else if (result < 0 && errno == ECHILD) {
        atomic_store(&ctx->child_reaped, 1);
    }
    return result;
}

/*
 * ============================================================================
 * Async Validation Lifecycle
 * ============================================================================
 *
 *   async_validate_config()
 *         |
 *         v
 *      fork()
 *         |
 *         v
 *   register pipe fd
 *         |
 *         v
 *   register timer fd
 *         |
 *         v
 *   wait for completion
 *    /                \
 *   IO complete       timeout
 *   (child exited)    (5s expired)
 *         |                |
 *         v                v
 *   validate_cleanup()
 *         |
 *         v
 *   callback(result, output)
 *
 * ============================================================================
 * Cleanup Once Contract
 *
 * 1. callback invoked exactly once
 * 2. resources released exactly once
 * 3. child reaped exactly once
 * 4. all FDs closed exactly once
 * 5. atomic_completed ensures no double cleanup
 *
 * ============================================================================
 */

/* ========== Public API ========== */

int async_validate_config(async_validate_ctx_t *ctx, reactor_t *r,
                          const char *bin_path, const char *conf_path, const char *work_dir,
                          validate_callback_t callback, void *userdata) {
    int pipe_fds[2] = {-1, -1};
    int timer_fd = -1;
    pid_t pid = -1;

    if (!ctx || !r || !bin_path || !callback) return -1;

    memset(ctx, 0, sizeof(async_validate_ctx_t));
    atomic_init(&ctx->completed, 0);
    atomic_init(&ctx->child_reaped, 0);
    ctx->child_status = 0;
    ctx->child_status_valid = 0;
    ctx->reactor = r;
    ctx->callback = callback;
    ctx->userdata = userdata;
    ctx->pipe_fd = -1;
    ctx->timer_fd = -1;
    ctx->child_pid = -1;
    ctx->output_len = 0;
    ctx->output_truncated = 0;
    ctx->output[0] = '\0';

    if (pipe2(pipe_fds, O_NONBLOCK | O_CLOEXEC) < 0) {
        LOG_ERROR("AsyncValidate: pipe failed: %s", strerror(errno));
        return -1;
    }

    pid = fork();
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

        if (conf_path && conf_path[0]) {
            execl(bin_path, bin_path, "check", "-D", work_dir, "-c", conf_path, NULL);
        } else {
            execl(bin_path, bin_path, "check", "-D", work_dir, NULL);
        }
        _exit(127);
    }

    /* Parent process */
    close(pipe_fds[1]);
    pipe_fds[1] = -1;
    ctx->child_pid = pid;
    ctx->pipe_fd = pipe_fds[0];

    /* Create timeout timer (5 seconds) */
    timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timer_fd < 0) {
        LOG_ERROR("AsyncValidate: timerfd_create failed: %s", strerror(errno));
        goto fail;
    }

    struct itimerspec its = {
        .it_value = { .tv_sec = 5, .tv_nsec = 0 },
        .it_interval = { .tv_sec = 0, .tv_nsec = 0 }
    };

    if (timerfd_settime(timer_fd, 0, &its, NULL) < 0) {
        LOG_ERROR("AsyncValidate: timerfd_settime failed: %s", strerror(errno));
        close(timer_fd);
        goto fail;
    }

    ctx->timer_fd = timer_fd;

    /* Register with reactor */
    if (reactor_add_fd(r, ctx->pipe_fd, REACTOR_EVENT_READ, validate_io_cb, ctx) != 0) {
        LOG_ERROR("AsyncValidate: reactor_add_fd pipe failed");
        goto fail;
    }

    if (reactor_add_fd(r, ctx->timer_fd, REACTOR_EVENT_READ, validate_timeout_cb, ctx) != 0) {
        LOG_ERROR("AsyncValidate: reactor_add_fd timer failed");
        reactor_remove_fd(r, ctx->pipe_fd);
        goto fail;
    }

    LOG_DEBUG("AsyncValidate: started (PID: %d)", pid);
    return 0;

fail:
    /* Unified failure cleanup */
    if (ctx->pipe_fd >= 0) {
        close(ctx->pipe_fd);
        ctx->pipe_fd = -1;
    }
    if (ctx->timer_fd >= 0) {
        close(ctx->timer_fd);
        ctx->timer_fd = -1;
    }
    if (ctx->child_pid > 0) {
        kill(ctx->child_pid, SIGKILL);
        (void)reap_child(ctx, 1, NULL);
        ctx->child_pid = -1;
    }
    return -1;
}

void async_validate_cleanup(async_validate_ctx_t *ctx) {
    if (!ctx) return;
    validate_cleanup(ctx, -1, "manual cleanup");
}

/* ========== Internal Callbacks ========== */

static void validate_io_cb(reactor_t *r, int fd, uint32_t events, void *userdata) {
    async_validate_ctx_t *ctx = userdata;
    (void)r;
    (void)events;

    if (!ctx) return;

    if (atomic_load(&ctx->completed)) {
        return;
    }

    char buf[1024];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            size_t room = sizeof(ctx->output) - 1 - ctx->output_len;
            size_t copy_len = (size_t)n < room ? (size_t)n : room;
            if (copy_len) {
                memcpy(ctx->output + ctx->output_len, buf, copy_len);
                ctx->output_len += copy_len;
                ctx->output[ctx->output_len] = '\0';
            }
            if (copy_len < (size_t)n && !ctx->output_truncated) {
                ctx->output_truncated = 1;
                LOG_WARN("AsyncValidate: output truncated");
            }
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (n == 0) {
            int status = 0;
            pid_t wr = reap_child(ctx, 0, &status);
            if (wr == 0) {
                LOG_DEBUG("AsyncValidate: EOF but child still running, waiting");
                return;
            }
            if (wr < 0) {
                LOG_ERROR("AsyncValidate: waitpid failed: %s", strerror(errno));
                validate_cleanup(ctx, 0, "waitpid error");
                return;
            }
            if (ctx->child_status_valid) status = ctx->child_status;
            int result = (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 1 : 0;
            validate_cleanup(ctx, result, ctx->output);
            return;
        }
        LOG_ERROR("AsyncValidate: read error: %s", strerror(errno));
        validate_cleanup(ctx, 0, "read error");
        return;
    }
}

static void validate_timeout_cb(reactor_t *r, int fd, uint32_t events, void *userdata) {
    async_validate_ctx_t *ctx = userdata;
    (void)r;
    (void)events;

    if (!ctx) return;

    if (atomic_load(&ctx->completed)) {
        return;
    }

    uint64_t expirations;
    ssize_t nr = read(fd, &expirations, sizeof(expirations));

    if (nr != sizeof(expirations)) {
        LOG_ERROR("AsyncValidate: timerfd read error: %s", strerror(errno));
        validate_cleanup(ctx, 0, "timerfd read error");
        return;
    }

    /* Check if child already exited */
    int status = 0;
    pid_t wr = reap_child(ctx, 0, &status);
    if (wr == ctx->child_pid) {
        /* Child already exited, wait for IO callback to handle */
        LOG_DEBUG("AsyncValidate: timeout but child already exited");
        return;
    }

    LOG_WARN("AsyncValidate: timeout, killing child (PID: %d)", ctx->child_pid);
    if (kill(ctx->child_pid, SIGKILL) < 0 && errno != ESRCH) {
        validate_cleanup(ctx, 0, "timeout kill error");
        return;
    }

    /* Reap child */
    (void)reap_child(ctx, 1, &status);

    validate_cleanup(ctx, 0, "timeout");
}

/*
 * Cleanup Once Contract
 *
 * callback invoked exactly once
 * resources released exactly once
 * child reaped exactly once
 */
static void validate_cleanup(async_validate_ctx_t *ctx, int result, const char *output) {
    int completed_old;

    if (!ctx) return;

    /* Atomic exchange ensures cleanup happens exactly once */
    completed_old = atomic_exchange(&ctx->completed, 1);
    if (completed_old) {
        LOG_DEBUG("AsyncValidate: cleanup already done");
        return;
    }

    LOG_DEBUG("AsyncValidate: cleanup, result=%d", result);

    /* Remove from reactor */
    if (ctx->reactor) {
        if (ctx->pipe_fd >= 0) {
            reactor_remove_fd(ctx->reactor, ctx->pipe_fd);
        }
        if (ctx->timer_fd >= 0) {
            reactor_remove_fd(ctx->reactor, ctx->timer_fd);
        }
    }

    /* Close pipe */
    if (ctx->pipe_fd >= 0) {
        close(ctx->pipe_fd);
        ctx->pipe_fd = -1;
    }

    /* Close timer */
    if (ctx->timer_fd >= 0) {
        close(ctx->timer_fd);
        ctx->timer_fd = -1;
    }

    /* Kill child if still running */
    if (ctx->child_pid > 0 && !atomic_load(&ctx->child_reaped)) {
        pid_t wr = reap_child(ctx, 0, NULL);
        if (wr == 0) {
            if (kill(ctx->child_pid, SIGKILL) < 0 && errno != ESRCH) {
                LOG_WARN("AsyncValidate: failed to kill child: %s", strerror(errno));
            }
            (void)reap_child(ctx, 1, NULL);
        }
        ctx->child_pid = -1;
    }

    /* Reset reactor reference */
    ctx->reactor = NULL;

    /* Invoke callback exactly once */
    if (ctx->callback) {
        ctx->callback(result, output, ctx->userdata);
    }
}
