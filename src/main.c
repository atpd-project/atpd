/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * Main entry point - Reactor mode
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
#include "app_filter.h"
#include "fcm_monitor.h"
#include "perf_mode.h"
#include "status.h"
#include "ui.h"
#include "cli.h"
#include "version.h"
#include "tproxy.h"
#include "geoip.h"
#include "boxbpf.h"
#include "cleanup.h"
#include "reactor.h"
#include "uds.h"
#include "singbox_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <libgen.h>

#define SAFE_PATH_MAX (PATH_MAX + 256)

#define g_config g_atpd.config
#define g_api_ctx g_atpd.api_ctx
#define g_reactor g_atpd.reactor
#define g_svc g_atpd.svc
#define g_running g_atpd.running
#define g_reload g_atpd.reload
#define g_show_status g_atpd.show_status

/* ========== Forward Declarations ========== */

static void run_event_loop(void);
static void on_signal(reactor_t *r, int sig, void *userdata);
static void on_idle(reactor_t *r, void *userdata);
static void service_stop_sync(service_ctx_t *ctx);
static int write_pid_file(const char *pid_file);
static void resolve_pid_path(atp_options_t *opts, char *pp, size_t size);
static void daemonize(void);
static void cleanup_ebpf(void);
static int process_is_atpd(pid_t pid);

/* Global PID file descriptor for fcntl lock */
static int g_pid_fd = -1;

/* ========== PID File & Process Checks ========== */

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

    snprintf(exe_copy, sizeof(exe_copy), "%s", exe);
    char *base = basename(exe_copy);
    if (!base) return 0;

    return strcmp(base, "atpd") == 0 ||
           strcmp(base, "atpd-zig") == 0;
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

    g_pid_fd = open(pid_file, O_RDWR | O_CREAT | O_NOFOLLOW, 0644);
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
        return -1;
    }

    if (ftruncate(g_pid_fd, 0) < 0) return -1;
    
    char buf[32];
    int buf_len = snprintf(buf, sizeof(buf), "%d\n", getpid());
    
    if (write(g_pid_fd, buf, buf_len) != buf_len) {
        fprintf(stderr, "Error: Failed to write PID to file\n");
        return -1;
    }

    return 0;
}

static void daemonize(void) {
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "Error: First fork failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
    if (pid > 0) exit(EXIT_SUCCESS);
    
    if (setsid() < 0) {
        fprintf(stderr, "Error: setsid failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "Error: Second fork failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
    if (pid > 0) exit(EXIT_SUCCESS);

    umask(0);
    
    int fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > 2) close(fd);
    }
    (void)chdir("/");
}

static void cleanup_ebpf(void) {
    if (g_config.ebpf.ready) {
        LOG_INFO("Cleaning up eBPF CNIP...");
        int ret = boxbpf_clear();
        if (ret != ATP_OK) {
            LOG_ERROR("Failed to clear eBPF: %d", ret);
        }
        g_config.ebpf.ready = 0;
        g_atpd_ctx.ebpf_enabled = false;
        atpd_ebpf_state_transition(EBPF_STATE_UNINITIALIZED);
    }
}

/* ========== Signal Handling ========== */

static void on_signal(reactor_t *r, int sig, void *userdata) {
    (void)r;
    (void)userdata;

    if (sig == SIGCHLD) {
        service_sigchld_cb(r, sig, g_svc);
        return;
    }

    if (sig == SIGHUP) {
        atpd_runtime_state_transition(ATPD_RUNTIME_STATE_RELOADING);
        g_reload = 1;
    } else if (sig == SIGUSR1) {
        g_show_status = 1;
    } else {
        g_running = 0;
    }
}

static void on_idle(reactor_t *r, void *userdata) {
    (void)r;
    (void)userdata;

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

/* ========== Proxy List ========== */

typedef struct {
    char last_name[256];
    int last_delay;
    bool first_run;
} proxy_log_throttle_t;

static proxy_log_throttle_t g_proxy_throttle = { .first_run = true };

static void process_proxy_list(proxy_list_t *list) {
    if (!list || list->count == 0) {
        proxy_list_free(list);
        return;
    }

    for (int i = 0; i < list->count; ++i) {
        proxy_info_t *info = &list->proxies[i];
        if (info->type && strcmp(info->type, "URLTest") == 0 && info->delay > 0) {
            bool changed = g_proxy_throttle.first_run ||
                          strcmp(g_proxy_throttle.last_name, info->name) != 0 ||
                          g_proxy_throttle.last_delay != info->delay;
            if (changed) {
                LOG_INFO("URLTest Node: %s (delay: %dms)", info->name, info->delay);
                snprintf(g_proxy_throttle.last_name, sizeof(g_proxy_throttle.last_name), "%s", info->name);
                g_proxy_throttle.last_delay = info->delay;
                g_proxy_throttle.first_run = false;
            }
        }
    }
    proxy_list_free(list);
}

static void on_proxies_response(int http_code, const char *body, void *userdata) {
    (void)userdata;
    if (http_code != 200 || !body) return;

    char *mutable_body = strdup(body);
    if (!mutable_body) return;

    proxy_list_t list = {0};
    if (singbox_parse_proxies(mutable_body, strlen(mutable_body), &list) >= 0) {
        process_proxy_list(&list);
    }
    free(mutable_body);
}

static void proxies_timer_cb(reactor_t *r, reactor_timer_t *timer, void *userdata) {
    (void)r;
    (void)timer;
    (void)userdata;
    api_get_proxies_async(&g_api_ctx, on_proxies_response, NULL);
}

/* ========== Netlink ========== */

static void netlink_io_cb(reactor_t *r, int fd, uint32_t events, void *userdata) {
    (void)r;
    (void)events;
    netlink_handle_event(fd, userdata);
}

/* ========== Service Stop Sync ========== */

static void service_stop_sync(service_ctx_t *ctx) {
    if (!ctx) return;

    LOG_INFO("Stopping service synchronously...");

    if (service_stop_async(ctx, NULL, NULL) != 0) {
        LOG_ERROR("Failed to stop service");
        return;
    }

    int waited = 0;
    while (waited < 5000) {
        if (!service_is_running(ctx)) {
            LOG_INFO("Service stopped");
            return;
        }
        usleep(100 * 1000);
        waited += 100;
    }

    LOG_WARN("Service stop timeout, forcing kill");
    kill(service_get_pid(ctx), SIGKILL);
}

/* ========== Event Loop ========== */

static void run_event_loop(void) {
    reactor_timer_t *proxy_timer = NULL;

    g_reactor = reactor_create();
    if (!g_reactor) {
        LOG_ERROR("Failed to create reactor");
        uds_cleanup();
        return;
    }

    netlink_set_reactor(g_reactor);
    uds_init(g_reactor, ATPD_UDS_PATH);

    api_start_with_reactor(&g_api_ctx, g_reactor);

    reactor_add_timer(g_reactor, 1000, 3000, service_monitor_cb, g_svc);

    if (g_svc && service_start_async(g_svc) != 0) {
        LOG_ERROR("Failed to start service");
        goto cleanup;
    }

    proxy_timer = reactor_add_timer(g_reactor, 10000, 30000, proxies_timer_cb, NULL);

    reactor_set_signal_cb(g_reactor, on_signal);
    reactor_set_idle_cb(g_reactor, on_idle);

    reactor_watch_signal(g_reactor, SIGINT);
    reactor_watch_signal(g_reactor, SIGTERM);
    reactor_watch_signal(g_reactor, SIGHUP);
    reactor_watch_signal(g_reactor, SIGUSR1);
    reactor_watch_signal(g_reactor, SIGCHLD);

    int nl_fd = netlink_get_fd();
    if (nl_fd >= 0) {
        reactor_add_fd(g_reactor, nl_fd, REACTOR_EVENT_READ, netlink_io_cb, NULL);
    }

    LOG_INFO("Reactor event loop started");
    reactor_run(g_reactor);
    LOG_INFO("Reactor event loop stopped");

cleanup:
    if (proxy_timer) {
        reactor_cancel_timer(g_reactor, proxy_timer);
        proxy_timer = NULL;
    }

    service_ctx_t *svc = g_svc;
    g_svc = NULL;
    if (svc) {
        service_stop_sync(svc);
        free(svc);
    }

    cleanup_ebpf();
    tproxy_cleanup_all(&g_config);
    netlink_cleanup();
    uds_cleanup();
    api_cleanup(&g_api_ctx);

    if (g_reactor) {
        reactor_destroy(g_reactor);
        g_reactor = NULL;
    }
}

/* ========== Commands ========== */

static int do_start(atp_options_t *opts) {
    char pp[SAFE_PATH_MAX];
    atpd_init_context_t init_ctx = {
        .config = &g_config,
        .ctx = &g_atpd_ctx,
        .reactor = NULL,
        .service = NULL,
        .api = &g_api_ctx,
        .opts = opts
    };

    LOG_INFO("Cleaning up stale rules before start...");
    boxbpf_clear();
    tproxy_cleanup_all(&g_config);

    resolve_pid_path(opts, pp, sizeof(pp));

    if (opts->daemon && !opts->foreground) {
        daemonize();
    }

    if (write_pid_file(pp) < 0) {
        return 1; 
    }

    atpd_context_init();

    if (atpd_init_run(&init_ctx) != 0) {
        LOG_ERROR("Initialization failed");
        atpd_runtime_state_transition(ATPD_RUNTIME_STATE_FAILED);
        
        cleanup_ebpf();
        netlink_cleanup();
        uds_cleanup();
        api_cleanup(&g_api_ctx);
        
        unlink(pp);
        return 1;
    }

    g_svc = init_ctx.service;
    atpd_runtime_state_transition(ATPD_RUNTIME_STATE_RUNNING);

    if (g_config.core.performance_mode) {
        perf_mode_init(&g_config);
        perf_mode_setup(&g_config);
    }

    netlink_set_tproxy_ready();
    run_event_loop();

    unlink(pp);
    return 0;
}

static int do_stop(atp_options_t *opts) {
    char pp[SAFE_PATH_MAX];
    int ret = 0;

    resolve_pid_path(opts, pp, sizeof(pp));

    FILE *f = fopen(pp, "r");
    if (!f) {
        fprintf(stderr, "Daemon is not running (no PID file)\n");
        return 1;
    }

    int pid;
    if (fscanf(f, "%d", &pid) != 1) {
        fprintf(stderr, "Invalid PID file\n");
        fclose(f);
        return 1;
    }
    fclose(f);

    if (!process_is_atpd(pid)) {
        fprintf(stderr, "Process %d is not ATP daemon\n", pid);
        unlink(pp);
        return 1;
    }

    if (kill(pid, 0) < 0) {
        fprintf(stderr, "Daemon is not running (stale PID file)\n");
        atp_cleanup_manual(&g_config);
        unlink(pp);
        return 0;
    }

    printf("Stopping daemon (PID: %d)...\n", pid);
    kill(pid, SIGTERM);

    for (int i = 0; i < SERVICE_STOP_RETRY_COUNT; i++) {
        if (kill(pid, 0) < 0) {
            printf("Daemon stopped\n");
            unlink(pp);
            boxbpf_clear();
            return 0;
        }
        usleep(SERVICE_STOP_INTERVAL_MS * 1000);
    }

    printf("Daemon not responding, forcing kill...\n");
    if (kill(pid, SIGKILL) == 0) {
        ret = 0;
    } else {
        ret = 1;
    }
    unlink(pp);
    boxbpf_clear();
    return ret;
}

static int do_status(atp_options_t *opts) {
    if (opts->no_color) ui_set_no_color(1);
    ui_init();

    service_ctx_t svc = {0};
    service_init(&svc, &g_config);
    api_init(&g_api_ctx, &g_config);

    netlink_init(NULL, &g_config);
    char vpn_iface[IFNAMSIZ] = {0};
    if (netlink_get_active_vpn(vpn_iface, sizeof(vpn_iface)) == 0 && vpn_iface[0]) {
        atpd_vpn_state_transition(VPN_STATE_READY, 0, vpn_iface);
    }
    status_show(&g_config, &svc, &g_api_ctx);

    netlink_cleanup();
    api_cleanup(&g_api_ctx);

    return 0;
}

static int do_reload(atp_options_t *opts) {
    char pp[SAFE_PATH_MAX];
    resolve_pid_path(opts, pp, sizeof(pp));

    FILE *f = fopen(pp, "r");
    if (!f) {
        fprintf(stderr, "Daemon is not running\n");
        return 1;
    }

    int pid;
    if (fscanf(f, "%d", &pid) != 1) {
        fprintf(stderr, "Invalid PID file\n");
        fclose(f);
        return 1;
    }
    fclose(f);

    if (!process_is_atpd(pid)) {
        fprintf(stderr, "Process %d is not ATP daemon\n", pid);
        return 1;
    }

    if (kill(pid, 0) < 0) {
        fprintf(stderr, "Daemon is not running\n");
        return 1;
    }

    if (kill(pid, SIGHUP) == 0) {
        printf("Reload signal sent to daemon (PID: %d)\n", pid);
        return 0;
    } else {
        fprintf(stderr, "Failed to send reload signal\n");
        return 1;
    }
}

static int do_check(atp_options_t *opts) {
    const char *config_path = opts->config_file;

    if (!config_path || !config_path[0]) {
        config_path = ATP_DEFAULT_DIR "/" ATP_CONF_FILE;
    }

    if (access(config_path, R_OK) != 0) {
        fprintf(stderr, "Config file not found: %s\n", config_path);
        return 1;
    }

    config_set_defaults(&g_config);
    if (config_load(config_path, &g_config) != ATP_OK) {
        fprintf(stderr, "Failed to load config\n");
        return 1;
    }

    printf("Config file: %s\n", config_path);
    printf("Configuration valid\n");
    return 0;
}

static int do_update_geoip(atp_options_t *opts) {
    (void)opts;
    if (!g_config.filter.bypass_cn_ip) {
        printf("CNIP bypass disabled, skipping update\n");
        return 0;
    }

    printf("Updating GeoIP database...\n");
    if (geoip_force_update(&g_config) == 0) {
        printf("GeoIP update completed successfully\n");
        return 0;
    } else {
        printf("GeoIP update failed\n");
        return 1;
    }
}

static int do_ebpf_probe(atp_options_t *opts) {
    int ret = boxbpf_probe(opts->ipv6);
    if (ret == ATP_OK) {
        printf("supported=1\nmessage=ok\nlpm_ipv4=1\nprogram_ipv4=1\npin_ipv4=1\n");
        if (opts->ipv6) {
            printf("lpm_ipv6=1\nprogram_ipv6=1\npin_ipv6=1\n");
        }
        return ATP_OK;
    } else {
        printf("supported=0\nmessage=eBPF xt_bpf unavailable\n");
        return ATP_ERR_EBPF;
    }
}

static int do_ebpf_init(atp_options_t *opts) {
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

    if (opts->ebpf_config[0] != '\0') {
        snprintf(cfg.ebpf.config_path, sizeof(cfg.ebpf.config_path),
                 "%s", opts->ebpf_config);
    }
    return boxbpf_init_from_config(&cfg);
}

static int do_ebpf_apply(atp_options_t *opts) {
    const char *path = opts->ebpf_config[0] ? opts->ebpf_config : "/data/adb/atp/ebpf/config.json";
    return boxbpf_apply(path);
}

static int do_ebpf_update(atp_options_t *opts) {
    const char *path = opts->ebpf_config[0] ? opts->ebpf_config : "/data/adb/atp/ebpf/config.json";
    return boxbpf_update(path);
}

static int do_ebpf_clear(atp_options_t *opts) {
    (void)opts;
    return boxbpf_clear();
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

    if (boxbpf_status(state, sizeof(state), &cfg) == ATP_OK) {
        printf("eBPF Status: %s\n", state);
        return ATP_OK;
    } else {
        printf("eBPF Status: UNINITIALIZED\n");
        return ATP_ERR_EBPF;
    }
}

/* ========== Main ========== */

int main(int argc, char *argv[]) {
    atp_options_t opts = {0};

    if (parse_arguments(argc, argv, &opts) != 0) {
        return 1;
    }

    config_set_defaults(&g_config);
    atpd_context_init();
    atpd_runtime_state_transition(ATPD_RUNTIME_STATE_INITIALIZING);

    switch (opts.command) {
        case CMD_START:
            return do_start(&opts);
        case CMD_STOP:
            return do_stop(&opts);
        case CMD_STATUS:
            return do_status(&opts);
        case CMD_RELOAD:
            return do_reload(&opts);
        case CMD_CHECK:
            return do_check(&opts);
        case CMD_UPDATE_GEOIP:
            return do_update_geoip(&opts);
        case CMD_VERSION:
            print_version();
            return 0;
        case CMD_HELP:
            print_usage(argv[0]);
            return 0;
        case CMD_EBPF_PROBE:
            return do_ebpf_probe(&opts);
        case CMD_EBPF_INIT:
            return do_ebpf_init(&opts);
        case CMD_EBPF_APPLY:
            return do_ebpf_apply(&opts);
        case CMD_EBPF_UPDATE:
            return do_ebpf_update(&opts);
        case CMD_EBPF_CLEAR:
            return do_ebpf_clear(&opts);
        case CMD_EBPF_STATUS:
            return do_ebpf_status(&opts);
        default:
            print_usage(argv[0]);
            return 0;
    }
}
