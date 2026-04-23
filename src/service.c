/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * Service Manager - Async Reactor-driven state machine
 * Zero sleep, zero exec_cmd, pure async
 */

#include "service.h"
#include "logger.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pwd.h>
#include <grp.h>

/* ========== Atomic Actions ========== */

static int service_is_alive(service_ctx_t *ctx) {
    if (ctx->child_pid <= 0) return 0;
    return kill(ctx->child_pid, 0) == 0;
}

static int service_probe_port(int port) {
    int sock = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (sock < 0) return 0;
    
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    
    int ret = connect(sock, (struct sockaddr *)&sa, sizeof(sa));
    close(sock);
    
    if (ret == 0) return 1;
    return (errno == EINPROGRESS) ? 1 : 0;
}

static int service_spawn(service_ctx_t *ctx) {
    pid_t pid = fork();
    if (pid < 0) {
        LOG_ERROR("Service: fork failed: %s", strerror(errno));
        return -1;
    }
    
    if (pid == 0) {
        umask(027);
        setsid();
        signal(SIGPIPE, SIG_IGN);
        
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        
        int log_fd = open(ctx->log_path, O_WRONLY | O_CREAT | O_APPEND, 0640);
        if (log_fd >= 0) {
            dup2(log_fd, STDOUT_FILENO);
            dup2(log_fd, STDERR_FILENO);
            close(log_fd);
        } else {
            int null_fd = open("/dev/null", O_RDWR);
            if (null_fd >= 0) {
                dup2(null_fd, STDIN_FILENO);
                dup2(null_fd, STDOUT_FILENO);
                dup2(null_fd, STDERR_FILENO);
                close(null_fd);
            }
        }
        
        struct passwd *pwd = getpwnam(ctx->user);
        struct group *grp = getgrnam(ctx->group);
        if (grp) setgid(grp->gr_gid);
        if (pwd) setuid(pwd->pw_uid);
        
        char *argv[] = { ctx->bin_path, "run", "-D", ctx->work_dir, NULL };
        execv(ctx->bin_path, argv);
        _exit(127);
    }
    
    ctx->child_pid = pid;
    LOG_INFO("Service: spawned sing-box (PID: %d)", pid);
    return 0;
}

static void service_kill(service_ctx_t *ctx) {
    if (ctx->child_pid > 0) {
        LOG_INFO("Service: stopping sing-box (PID: %d)", ctx->child_pid);
        kill(ctx->child_pid, SIGTERM);
        usleep(500000);
        if (kill(ctx->child_pid, 0) == 0) {
            kill(ctx->child_pid, SIGKILL);
        }
        waitpid(ctx->child_pid, NULL, WNOHANG);
        ctx->child_pid = -1;
    }
}

/* ========== State Machine ========== */

const char *service_state_string(service_state_t state) {
    switch (state) {
        case SERVICE_STOPPED:  return "STOPPED";
        case SERVICE_STARTING: return "STARTING";
        case SERVICE_RUNNING:  return "RUNNING";
        case SERVICE_FAILED:   return "FAILED";
        default:               return "UNKNOWN";
    }
}

void service_monitor_cb(reactor_t *r, reactor_timer_t *timer, void *userdata) {
    (void)r;
    (void)timer;
    
    service_ctx_t *ctx = userdata;
    if (!ctx) return;
    
    switch (ctx->state) {
        case SERVICE_STOPPED:
            break;
            
        case SERVICE_STARTING:
            if (service_is_alive(ctx)) {
                if (service_probe_port(ctx->api_port)) {
                    LOG_INFO("Service: ready on port %d", ctx->api_port);
                    ctx->state = SERVICE_RUNNING;
                    ctx->fail_count = 0;
                }
            } else {
                LOG_WARN("Service: process died during startup");
                ctx->fail_count++;
                if (ctx->fail_count >= ctx->max_failures) {
                    LOG_ERROR("Service: max failures reached, giving up");
                    ctx->state = SERVICE_FAILED;
                } else {
                    LOG_INFO("Service: respawning (%d/%d)", ctx->fail_count, ctx->max_failures);
                    service_spawn(ctx);
                }
            }
            break;
            
        case SERVICE_RUNNING:
            if (!service_is_alive(ctx)) {
                LOG_WARN("Service: process died unexpectedly");
                ctx->fail_count++;
                if (ctx->fail_count >= ctx->max_failures) {
                    LOG_ERROR("Service: max failures reached, giving up");
                    ctx->state = SERVICE_FAILED;
                } else {
                    LOG_INFO("Service: auto-restarting (%d/%d)", ctx->fail_count, ctx->max_failures);
                    service_spawn(ctx);
                    ctx->state = SERVICE_STARTING;
                }
            }
            break;
            
        case SERVICE_FAILED:
            break;
    }
}

/* ========== Public API ========== */

int service_init(service_ctx_t *ctx, atp_config_t *cfg) {
    if (!ctx || !cfg) return -1;
    memset(ctx, 0, sizeof(service_ctx_t));
    
    snprintf(ctx->bin_path, sizeof(ctx->bin_path), "%.4000s/bin/%.63s", 
             cfg->data_dir, PROXY_BIN_NAME);
    snprintf(ctx->work_dir, sizeof(ctx->work_dir), "%.4000s/sing-box", cfg->data_dir);
    snprintf(ctx->conf_path, sizeof(ctx->conf_path), "%.4000s/sing-box/config.json", cfg->data_dir);
    snprintf(ctx->log_path, sizeof(ctx->log_path), "%.4000s/run/sing-box.log", cfg->data_dir);
    snprintf(ctx->user, sizeof(ctx->user), "%.63s", cfg->core_user);
    snprintf(ctx->group, sizeof(ctx->group), "%.63s", cfg->core_group);
    
    ctx->api_port = cfg->api_port;
    ctx->child_pid = -1;
    ctx->state = SERVICE_STOPPED;
    ctx->fail_count = 0;
    ctx->max_failures = 3;
    
    LOG_DEBUG("Service: initialized (bin=%s, port=%d)", ctx->bin_path, ctx->api_port);
    return 0;
}

int service_start_async(service_ctx_t *ctx) {
    if (!ctx) return -1;
    
    if (ctx->state == SERVICE_RUNNING || ctx->state == SERVICE_STARTING) {
        LOG_WARN("Service: already running or starting");
        return 0;
    }
    
    if (service_is_alive(ctx)) {
        LOG_INFO("Service: process already running (PID: %d)", ctx->child_pid);
        ctx->state = SERVICE_STARTING;
        return 0;
    }
    
    if (service_spawn(ctx) < 0) {
        ctx->state = SERVICE_FAILED;
        return -1;
    }
    
    ctx->state = SERVICE_STARTING;
    ctx->fail_count = 0;
    return 0;
}

int service_stop_async(service_ctx_t *ctx) {
    if (!ctx) return -1;
    
    service_kill(ctx);
    ctx->state = SERVICE_STOPPED;
    ctx->fail_count = 0;
    
    if (ctx->monitor_timer) {
        reactor_cancel_timer(ctx->reactor, ctx->monitor_timer);
        ctx->monitor_timer = NULL;
    }
    
    return 0;
}

int service_is_running(service_ctx_t *ctx) {
    return ctx && ctx->state == SERVICE_RUNNING && service_is_alive(ctx);
}
