/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * Main entry point - Pure eBPF Reactor Architecture
 */

#include "atp.h"
#include "atpd_global.h"
#include "atpd_context.h"
#include "atpd_init.h"
#include "logger.h"
#include "config.h"
#include "utils.h"
#include "service.h"
#include "api.h"
#include "netlink.h"
#include "perf_mode.h"
#include "status.h"
#include "ui.h"
#include "cli.h"
#include "version.h"
#include "ebpf.h"
#include "cleanup.h"
#include "reactor.h"
#include "uds.h"
#include "singbox_api.h"
#include "config_validator.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <errno.h>
#include <libgen.h>

#define SAFE_PATH_MAX (PATH_MAX + 256)
#define SIGNAL_RETRY_MAX 5
#define SIGNAL_RETRY_DELAY_US 10000

#define g_config g_atpd.config
#define g_api_ctx g_atpd.api_ctx
#define g_reactor g_atpd.reactor
#define g_svc g_atpd.svc
#define g_running g_atpd.running
#define g_reload g_atpd.reload
#define g_show_status g_atpd.show_status

static volatile sig_atomic_t g_signal_pending = 0;
static volatile sig_atomic_t g_signal_code = 0;

static void run_event_loop(void);
static void on_signal(reactor_t *r, int sig, void *userdata);
static void on_idle(reactor_t *r, void *userdata);
static void service_stop_sync(service_ctx_t *ctx);
static int write_pid_file(const char *pid_file);
static void resolve_pid_path(atp_options_t *opts, char *pp, size_t size);
static void daemonize(void);
static int process_is_atpd(pid_t pid);
static int verify_pid_file_unchanged(int fd, int expected_pid);

static int g_pid_fd = -1;

static void resolve_pid_path(atp_options_t *opts, char *pp, size_t size) {
    if (opts->pid_file[0]) {
        snprintf(pp, size, "%s", opts->pid_file);
    } else if (g_config.core.data_dir[0]) {
        snprintf(pp, size, "%s/%s", g_config.core.data_dir, ATP_PID_FILE);
    } else {
        snprintf(pp, size, "./atpd.pid");
    }
}

static int process_is_atpd(pid_t pid) {
    char path[64];
    char exe[PATH_MAX];
    char exe_copy[PATH_MAX];

    snprintf(path, sizeof(path), "/proc/%d/exe", pid);

    ssize_t len;
    int retry = 3;

    while (retry-- > 0) {
        len = readlink(path, exe, sizeof(exe) - 1);
        if (len >= 0) break;
        if (errno != EINTR) return 0;
    }

    if (len <= 0) return 0;
    exe[len] = '\0';

    strncpy(exe_copy, exe, sizeof(exe_copy) - 1);
    exe_copy[sizeof(exe_copy) - 1] = '\0';

    char *base = basename(exe_copy);
    if (!base) return 0;

    return strncmp(base, "atpd", 4) == 0;
}

static int verify_pid_file_unchanged(int fd, int expected_pid) {
    struct flock fl = {
        .l_type = F_RDLCK,
        .l_whence = SEEK_SET,
        .l_start = 0,
        .l_len = 0
    };

    if (fcntl(fd, F_SETLKW, &fl) < 0) {
        return -1;
    }

    char buf[32] = {0};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    int current_pid = 0;

    if (n > 0 && sscanf(buf, "%d", &current_pid) == 1) {
        fl.l_type = F_UNLCK;
        fcntl(fd, F_SETLK, &fl);
        return (current_pid == expected_pid) ? 0 : -1;
    }

    fl.l_type = F_UNLCK;
    fcntl(fd, F_SETLK, &fl);
    return -1;
}

static int write_pid_file(const char *pid_file) {
    char dir[SAFE_PATH_MAX];

    if (!pid_file) return -1;

    int len = snprintf(dir, sizeof(dir), "%s", pid_file);
    if (len < 0 || (size_t)len >= sizeof(dir)) {
        fprintf(stderr, "Error: PID file path too long\n");
        return -1;
    }

    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        if (mkdir_recursive(dir, 0755) != 0 && errno != EEXIST) {
            fprintf(stderr, "Error: Failed to create PID directory: %s\n", strerror(errno));
            return -1;
        }
    }

    g_pid_fd = open(pid_file, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (g_pid_fd < 0) {
        fprintf(stderr, "Error: Failed to open PID file: %s\n", strerror(errno));
        return -1;
    }

    struct flock fl = {
        .l_type = F_WRLCK,
        .l_whence = SEEK_SET,
        .l_start = 0,
        .l_len = 0
    };

    if (fcntl(g_pid_fd, F_SETLK, &fl) < 0) {
        if (errno == EACCES || errno == EAGAIN) {
            fprintf(stderr, "Daemon is already running (PID file locked by another instance)\n");
        } else {
            fprintf(stderr, "Error: Failed to lock PID file: %s\n", strerror(errno));
        }
        close(g_pid_fd);
        g_pid_fd = -1;
        return -1;
    }

    if (ftruncate(g_pid_fd, 0) < 0) {
        close(g_pid_fd);
        g_pid_fd = -1;
        return -1;
    }

    char buf[32];
    int buf_len = snprintf(buf, sizeof(buf), "%d\n", getpid());

    if (write(g_pid_fd, buf, buf_len) != buf_len) {
        fprintf(stderr, "Error: Failed to write PID to file\n");
        close(g_pid_fd);
        g_pid_fd = -1;
        return -1;
    }

    fsync(g_pid_fd);
    return 0;
}

static void daemonize(void) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(1);
    }
    if (pid > 0) exit(0);

    if (setsid() < 0) {
        perror("setsid");
        exit(1);
    }

    signal(SIGHUP, SIG_IGN);

    pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(1);
    }
    if (pid > 0) exit(0);

    umask(027);

    int null_fd = open("/dev/null", O_RDWR);
    if (null_fd >= 0) {
        dup2(null_fd, STDIN_FILENO);
        dup2(null_fd, STDOUT_FILENO);
        dup2(null_fd, STDERR_FILENO);
        if (null_fd > 2) close(null_fd);
    }
}

static void on_signal(reactor_t *r, int sig, void *userdata) {
    (void)r;
    (void)userdata;
    g_signal_pending = 1;
    g_signal_code = sig;
}

static void on_idle(reactor_t *r, void *userdata) {
    (void)r;
    (void)userdata;

    if (g_signal_pending) {
        int sig = g_signal_code;
        g_signal_pending = 0;

        if (sig == SIGCHLD) {
            service_sigchld_cb(r, sig, g_svc);
        } else if (sig == SIGHUP) {
            atpd_runtime_state_transition(ATPD_RUNTIME_STATE_RELOADING);
            g_reload = 1;
        } else if (sig == SIGUSR1) {
            g_show_status = 1;
        } else {
            g_running = 0;
        }
    }

    if (g_reload) {
        g_reload = 0;
        LOG_INFO("Processing config reload...");
        if (config_reload(&g_config) != ATP_OK) {
            LOG_ERROR("Config reload failed");
            atpd_runtime_state_transition(ATPD_RUNTIME_STATE_FAILED);
        } else {
            LOG_INFO("Config reload completed successfully");
            atpd_runtime_state_transition(ATPD_RUNTIME_STATE_RUNNING);
        }
    }

    if (g_show_status) {
        g_show_status = 0;
        LOG_INFO("Processing status display...");
        status_show(&g_config, g_svc, &g_api_ctx);
    }

    if (!g_running) {
        LOG_INFO("Stopping reactor...");
        atpd_runtime_state_transition(ATPD_RUNTIME_STATE_STOPPED);
        reactor_stop(r);
    }
}

static void service_stop_sync(service_ctx_t *ctx) {
    if (!ctx) return;
    int pid = service_get_pid(ctx);
    if (pid <= 0) return;

    LOG_INFO("Stopping sing-box core (PID %d)...", pid);
    kill(pid, SIGTERM);

    for (int i = 0; i < SERVICE_STOP_RETRY_COUNT; ++i) {
        usleep(SERVICE_STOP_INTERVAL_MS * 1000);
        if (kill(pid, 0) != 0 && errno == ESRCH) {
            LOG_INFO("Service stopped gracefully");
            return;
        }
    }

    LOG_WARN("Service did not stop gracefully, sending SIGKILL");
    kill(pid, SIGKILL);
    usleep(100000);
}

static void run_event_loop(void) {
    LOG_INFO("Initializing Pure eBPF Reactor event loop...");

    g_reactor = reactor_create();
    if (!g_reactor) {
        LOG_ERROR("Failed to create reactor");
        return;
    }

    reactor_set_signal_cb(g_reactor, on_signal);
    reactor_set_idle_cb(g_reactor, on_idle);

    int nl_fd = netlink_get_fd();
    if (nl_fd >= 0) {
        reactor_add_fd(g_reactor, nl_fd, REACTOR_EVENT_READ, netlink_handle_event, NULL);
    }

    netlink_set_reactor(g_reactor);

    char uds_path[SAFE_PATH_MAX];
    const char *data_dir = g_config.core.data_dir[0] ? g_config.core.data_dir : ATP_DEFAULT_DIR;
    snprintf(uds_path, sizeof(uds_path), "%s/%s", data_dir, ATP_COMMAND_SOCKET);
    if (uds_init(g_reactor, uds_path) < 0) {
        LOG_WARN("Failed to initialize UDS command socket");
    }

    if (service_start_async(g_svc) < 0) {
        LOG_ERROR("Failed to start service");
        reactor_destroy(g_reactor);
        g_reactor = NULL;
        return;
    }

    g_running = 1;
    LOG_INFO("Pure eBPF Reactor running, entering event loop");
    reactor_run(g_reactor);

    LOG_INFO("Reactor exited, cleaning up...");
    uds_cleanup();
    service_stop_sync(g_svc);

    if (g_reactor) {
        reactor_destroy(g_reactor);
        g_reactor = NULL;
    }
}

static int do_start(atp_options_t *opts) {
    char pp[SAFE_PATH_MAX];
    int ret = 0;
    bool pid_written = false;

    atpd_init_context_t init_ctx = {
        .config = &g_config,
        .ctx = &g_atpd_ctx,
        .reactor = NULL,
        .service = NULL,
        .api = &g_api_ctx,
        .opts = opts
    };

    resolve_pid_path(opts, pp, sizeof(pp));

    if (opts->daemon && !opts->foreground) {
        daemonize();
    }

    if (write_pid_file(pp) < 0) {
        ret = 1;
        goto cleanup_return;
    }
    pid_written = true;

    atpd_context_init();

    if (atpd_init_run(&init_ctx) != 0) {
        LOG_ERROR("Initialization failed");
        atpd_runtime_state_transition(ATPD_RUNTIME_STATE_FAILED);
        ret = 1;
        goto cleanup;
    }

    g_svc = init_ctx.service;
    atpd_runtime_state_transition(ATPD_RUNTIME_STATE_RUNNING);

    if (g_config.core.performance_mode) {
        perf_mode_init(&g_config);
        perf_mode_setup(&g_config);
    }

    LOG_INFO("Engine: Pure eBPF active (Zero iptables / sing-box native inbound)");

    run_event_loop();
    ret = 0;

cleanup:
    netlink_cleanup();
    uds_cleanup();
    api_cleanup(&g_api_ctx);
    if (g_svc) {
        service_stop_async(g_svc, NULL, NULL);
        free(g_svc);
        g_svc = NULL;
    }

cleanup_return:
    if (pid_written) {
        unlink(pp);
    }
    return ret;
}

static int do_stop(atp_options_t *opts) {
    char pp[SAFE_PATH_MAX];
    int ret = 0;
    int pid_file_fd = -1;

    resolve_pid_path(opts, pp, sizeof(pp));

    pid_file_fd = open(pp, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (pid_file_fd < 0) {
        fprintf(stderr, "Daemon is not running (no PID file)\n");
        return 1;
    }

    struct flock fl = {
        .l_type = F_RDLCK,
        .l_whence = SEEK_SET,
        .l_start = 0,
        .l_len = 0
    };

    if (fcntl(pid_file_fd, F_SETLKW, &fl) < 0) {
        close(pid_file_fd);
        fprintf(stderr, "Failed to lock PID file\n");
        return 1;
    }

    char buf[32] = {0};
    ssize_t n = read(pid_file_fd, buf, sizeof(buf) - 1);
    if (n <= 0) {
        fl.l_type = F_UNLCK;
        fcntl(pid_file_fd, F_SETLK, &fl);
        close(pid_file_fd);
        fprintf(stderr, "Failed to read PID from file\n");
        return 1;
    }

    pid_t pid = (pid_t)atoi(buf);
    if (pid <= 0) {
        fl.l_type = F_UNLCK;
        fcntl(pid_file_fd, F_SETLK, &fl);
        close(pid_file_fd);
        fprintf(stderr, "Invalid PID: %d\n", pid);
        return 1;
    }

    if (!process_is_atpd(pid)) {
        fl.l_type = F_UNLCK;
        fcntl(pid_file_fd, F_SETLK, &fl);
        close(pid_file_fd);
        fprintf(stderr, "Process %d is not atpd (stale PID file)\n", pid);
        unlink(pp);
        return 1;
    }

    if (kill(pid, SIGTERM) < 0) {
        if (errno == ESRCH) {
            fprintf(stderr, "Process %d not found (stale PID file)\n", pid);
            unlink(pp);
            ret = 1;
        } else {
            perror("kill");
            ret = 1;
        }
        fl.l_type = F_UNLCK;
        fcntl(pid_file_fd, F_SETLK, &fl);
        close(pid_file_fd);
        return ret;
    }

    fl.l_type = F_UNLCK;
    fcntl(pid_file_fd, F_SETLK, &fl);

    int stopped = 0;
    for (int i = 0; i < 50; i++) {
        usleep(100000);
        if (kill(pid, 0) != 0 && errno == ESRCH) {
            stopped = 1;
            break;
        }
    }

    if (!stopped) {
        if (verify_pid_file_unchanged(pid_file_fd, pid) == 0) {
            fprintf(stderr, "Process %d did not terminate gracefully, sending SIGKILL\n", pid);
            kill(pid, SIGKILL);
            usleep(100000);
        } else {
            fprintf(stderr, "PID file was modified, aborting SIGKILL\n");
        }
    }

    close(pid_file_fd);
    unlink(pp);

    printf("Daemon stopped successfully\n");
    return 0;
}

static int do_restart(atp_options_t *opts) {
    printf("Restarting atpd (Pure eBPF)...\n");
    do_stop(opts);
    usleep(500000);
    return do_start(opts);
}

static int do_status(atp_options_t *opts) {
    (void)opts;
    service_ctx_t local_svc;
    memset(&local_svc, 0, sizeof(local_svc));
    service_init(&local_svc, &g_config);

    api_ctx_t local_api;
    memset(&local_api, 0, sizeof(local_api));
    api_init(&local_api, &g_config);

    status_show(&g_config, &local_svc, &local_api);
    return 0;
}

static int do_reload(atp_options_t *opts) {
    char pp[SAFE_PATH_MAX];
    resolve_pid_path(opts, pp, sizeof(pp));

    FILE *fp = fopen(pp, "r");
    if (!fp) {
        fprintf(stderr, "Daemon is not running (no PID file)\n");
        return 1;
    }

    char buf[32] = {0};
    if (!fgets(buf, sizeof(buf), fp)) {
        fclose(fp);
        fprintf(stderr, "Failed to read PID file\n");
        return 1;
    }
    fclose(fp);

    pid_t pid = (pid_t)atoi(buf);
    if (pid <= 0 || !process_is_atpd(pid)) {
        fprintf(stderr, "Invalid or stale PID: %d\n", pid);
        return 1;
    }

    if (kill(pid, SIGHUP) < 0) {
        perror("kill(SIGHUP)");
        return 1;
    }

    printf("Reload signal sent to PID %d\n", pid);
    return 0;
}

static int do_check(atp_options_t *opts) {
    (void)opts;
    printf("Validating configuration for Pure eBPF...\n");
    if (config_validate_values(&g_config) == 0) {
        printf("Configuration is valid\n");
        return 0;
    } else {
        printf("Configuration validation failed\n");
        return 1;
    }
}

static int do_ebpf_probe(atp_options_t *opts) {
    ebpf_probe_result_t res;
    ebpf_probe_detailed(&res, opts->ipv6);
    printf("kernel_release=%s\n", res.kernel_release);
    printf("supported=%d\n", res.supported ? 1 : 0);
    printf("cgroup_sock_addr=%d\n", res.has_cgroup_sock_addr ? 1 : 0);
    printf("sched_cls=%d\n", res.has_sched_cls ? 1 : 0);
    printf("lpm_trie=%d\n", res.has_lpm_trie ? 1 : 0);
    printf("array=%d\n", res.has_array ? 1 : 0);
    printf("hash=%d\n", res.has_hash ? 1 : 0);
    printf("lru_hash=%d\n", res.has_lru_hash ? 1 : 0);
    return res.supported ? ATP_OK : ATP_ERR_EBPF;
}

static int do_ebpf_status(atp_options_t *opts) {
    char state[64] = {0};
    atp_config_t cfg;
    config_set_defaults(&cfg);

    const char *config_path = opts->config_file;
    if (config_path && config_path[0]) {
        config_load(config_path, &cfg);
    } else {
        const char *default_path = ATP_DEFAULT_DIR "/" ATP_CONF_FILE;
        if (access(default_path, R_OK) == 0) {
            config_load(default_path, &cfg);
        }
    }

    ebpf_status(state, sizeof(state), &cfg);
    printf("eBPF Data Path: sing-box native (cgroup.bpf.c)\n");
    printf("eBPF Kernel Support: %s\n", state);
    return ATP_OK;
}

int main(int argc, char *argv[]) {
    atp_options_t opts = {0};

    if (parse_arguments(argc, argv, &opts) != 0) {
        return 1;
    }

    config_set_defaults(&g_config);
    char auto_cfg_path[PATH_MAX];
    const char *cfg_path = opts.config_file[0] ? opts.config_file : NULL;
    if (!cfg_path) {
        snprintf(auto_cfg_path, sizeof(auto_cfg_path), "%s/%s", g_config.core.data_dir, ATP_CONF_FILE);
        cfg_path = auto_cfg_path;
    }
    if (access(cfg_path, R_OK) == 0) {
        config_load(cfg_path, &g_config);
    }
    g_config.core.foreground = opts.foreground;
    g_config.core.verbose = opts.verbose;

    atpd_context_init();
    atpd_runtime_state_transition(ATPD_RUNTIME_STATE_INITIALIZING);

    switch (opts.command) {
        case CMD_START:
            return do_start(&opts);
        case CMD_STOP:
            return do_stop(&opts);
        case CMD_RESTART:
            return do_restart(&opts);
        case CMD_STATUS:
            return do_status(&opts);
        case CMD_RELOAD:
            return do_reload(&opts);
        case CMD_CHECK:
            return do_check(&opts);
        case CMD_VERSION:
            print_version();
            return 0;
        case CMD_HELP:
            print_usage(argv[0]);
            return 0;
        case CMD_EBPF_PROBE:
            return do_ebpf_probe(&opts);
        case CMD_EBPF_STATUS:
            return do_ebpf_status(&opts);
        default:
            print_usage(argv[0]);
            return 0;
    }
}
