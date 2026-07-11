/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * Main entry point - Reactor mode
 */

#include "atp.h"
#include "config.h"
#include "config_validator.h"
#include "logger.h"
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
#include "routing.h"
#include "geoip.h"
#include "mac_filter.h"
#include "ipv6_manager.h"
#include "inet_diag.h"
#include "reactor.h"
#include "singbox_api.h"
#include "atpd_context.h"
#include "uds.h"
#include "boxbpf.h"
#include "cleanup.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <libgen.h>
#include <signal.h>

#define SAFE_PATH_MAX (PATH_MAX + 256)

api_ctx_t g_api_ctx;
atp_config_t g_config;
reactor_t *g_reactor = NULL;
static service_ctx_t *g_svc = NULL;
static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_reload = 0;
static volatile sig_atomic_t g_show_status = 0;

typedef struct {
    char last_name[256];
    int last_delay;
    bool first_run;
} proxy_log_throttle_t;

static proxy_log_throttle_t g_proxy_throttle = { .first_run = true };

static void on_signal(reactor_t *r, int sig, void *userdata) {
    (void)r;
    (void)userdata;

    if (sig == SIGCHLD) {
        service_sigchld_cb(r, sig, g_svc);
        return;
    }

    if (sig == SIGHUP) {
        g_reload = 1;
        LOG_INFO("Reload signal received");
    } else if (sig == SIGUSR1) {
        g_show_status = 1;
        LOG_INFO("Status signal received");
    } else {
        LOG_INFO("Termination signal received");
        g_running = 0;
    }
}

static void on_idle(reactor_t *r, void *userdata) {
    (void)r;
    (void)userdata;

    if (g_reload) {
        config_reload(&g_config);
        if (g_config.app_proxy_enable) {
            app_filter_reload(&g_config);
        }
        g_reload = 0;
    }

    if (g_show_status) {
        status_show(&g_config, g_svc, &g_api_ctx);
        g_show_status = 0;
    }

    if (!g_running) {
        reactor_stop(r);
    }
}

static void process_proxy_list(proxy_list_t *list) {
    if (!list || list->count == 0) {
        goto cleanup;
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

cleanup:
    proxy_list_free(list);
}

static void on_proxies_response(int http_code, const char *body, void *userdata) {
    (void)userdata;

    if (http_code != 200 || !body) return;

    char *mutable_body = strdup(body);
    if (!mutable_body) return;

    proxy_list_t list;
    int count = singbox_parse_proxies(mutable_body, strlen(mutable_body), &list);

    if (count >= 0) {
        process_proxy_list(&list);
    } else {
        proxy_list_free(&list);
    }

    free(mutable_body);
}

static void proxies_timer_cb(reactor_t *r, reactor_timer_t *timer, void *userdata) {
    (void)r;
    (void)timer;
    (void)userdata;
    api_get_proxies_async(&g_api_ctx, on_proxies_response, NULL);
}

static void netlink_io_cb(reactor_t *r, int fd, uint32_t events, void *userdata) {
    (void)r;
    (void)events;
    netlink_handle_event(fd, userdata);
}

static void service_stop_done_cb(service_ctx_t *ctx, void *userdata) {
    (void)userdata;
    LOG_INFO("Service cleanup complete");
    free(ctx);
    g_svc = NULL;
}

static void run_event_loop(void) {
    g_reactor = reactor_create();
    if (!g_reactor) {
        LOG_ERROR("Failed to create reactor");
        return;
    }

    netlink_set_reactor(g_reactor);
    uds_init(g_reactor, ATPD_UDS_PATH);

    api_start_with_reactor(&g_api_ctx, g_reactor);

    reactor_add_timer(g_reactor, 1000, 3000, service_monitor_cb, g_svc);
    service_start_async(g_svc);

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

    if (g_config.ebpf_ready) {
        LOG_INFO("Cleaning up eBPF CNIP...");
        boxbpf_clear();
        g_config.ebpf_ready = 0;
        g_atpd_ctx.ebpf_enabled = false;
        atpd_ebpf_state_transition(EBPF_STATE_UNINITIALIZED);
    }

    tproxy_cleanup_all(&g_config);
    uds_cleanup();

    if (g_svc) {
        service_stop_async(g_svc, service_stop_done_cb, NULL);
    }

    reactor_destroy(g_reactor);
    g_reactor = NULL;
}

static void daemonize(void) {
    pid_t pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS);
    if (setsid() < 0) exit(EXIT_FAILURE);
    pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
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

static int write_pid_file(const char *pid_file) {
    char dir[SAFE_PATH_MAX];
    if (!pid_file || strlen(pid_file) >= PATH_MAX) return -1;
    snprintf(dir, sizeof(dir), "%s", pid_file);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        mkdir_recursive(dir, 0755);
    }

    struct stat st;
    if (lstat(pid_file, &st) == 0) {
        if (!S_ISREG(st.st_mode)) {
            LOG_ERROR("PID file is not a regular file: %s", pid_file);
            return -1;
        }
        if (st.st_uid != getuid()) {
            LOG_WARN("PID file owned by different user: %s", pid_file);
        }
        if (st.st_nlink > 1) {
            LOG_ERROR("PID file has hard links, refusing to write: %s", pid_file);
            return -1;
        }
    }

    int fd = open(pid_file, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
    if (fd < 0) {
        if (errno == EEXIST) {
            return 0;
        }
        LOG_ERROR("Failed to create PID file: %s", strerror(errno));
        return -1;
    }

    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d\n", getpid());
    if (write(fd, buf, len) != len) {
        LOG_ERROR("Failed to write PID file: %s", strerror(errno));
        close(fd);
        unlink(pid_file);
        return -1;
    }
    close(fd);
    return 0;
}

static void get_binary_dir(char *buf, size_t size) {
    ssize_t len = readlink("/proc/self/exe", buf, size - 1);
    if (len <= 0) {
        buf[0] = '\0';
        return;
    }
    buf[len] = '\0';

    char *last_slash = strrchr(buf, '/');
    if (last_slash) {
        *last_slash = '\0';
    } else {
        buf[0] = '\0';
    }
}

static int find_config_file(char *path, size_t size, const char *user_path) {
    if (user_path && user_path[0]) {
        if (strlen(user_path) >= size) return -1;
        snprintf(path, size, "%s", user_path);
    } else {
        char bin_dir[PATH_MAX] = {0};
        get_binary_dir(bin_dir, sizeof(bin_dir));
        size_t dlen = strlen(bin_dir);
        if (dlen > 0 && dlen < (size - 16)) {
            memcpy(path, bin_dir, dlen);
            path[dlen] = '/';
            memcpy(path + dlen + 1, "atp.conf", 9);
        } else {
            snprintf(path, size, "./atp.conf");
        }
    }
    return access(path, R_OK);
}

static int confirm_operation(const char *op, int force) {
    if (force) return 1;
    char res[8];
    fprintf(stderr, "Warning: This will %s the ATP daemon. Are you sure? [y/N] ", op);
    if (!fgets(res, sizeof(res), stdin)) return 0;
    return (res[0] == 'y' || res[0] == 'Y');
}

static void resolve_pid_path(atp_options_t *opts, char *pp, size_t size) {
    if (opts->pid_file[0]) {
        snprintf(pp, size, "%s", opts->pid_file);
    } else if (g_config.data_dir[0]) {
        snprintf(pp, size, "%s/%s", g_config.data_dir, ATP_PID_FILE);
    } else {
        snprintf(pp, size, "./atpd.pid");
    }
}

static void cleanup_ebpf(void) {
    if (g_config.ebpf_ready) {
        LOG_INFO("Cleaning up eBPF CNIP...");
        boxbpf_clear();
        g_config.ebpf_ready = 0;
        g_atpd_ctx.ebpf_enabled = false;
        atpd_ebpf_state_transition(EBPF_STATE_UNINITIALIZED);
    }
}

static int do_start(atp_options_t *opts) {
    char cp[SAFE_PATH_MAX];
    char pp[SAFE_PATH_MAX];

    if (find_config_file(cp, SAFE_PATH_MAX, opts->config_file) != ATP_OK) {
        fprintf(stderr, "Config file not found\n");
        return 1;
    }

    config_set_defaults(&g_config);
    g_config.foreground = opts->foreground;
    g_config.verbose = opts->verbose;
    config_load(cp, &g_config);

    logger_init();
    log_set_level(opts->log_level);
    if (opts->no_color) log_set_color(0);

    LOG_INFO("Cleaning up stale rules before start...");
    boxbpf_clear();
    tproxy_cleanup_all(&g_config);

    resolve_pid_path(opts, pp, sizeof(pp));

    if (file_exists(pp)) {
        FILE *f = fopen(pp, "r");
        if (f) {
            int pid;
            if (fscanf(f, "%d", &pid) == 1 && kill(pid, 0) == 0) {
                fprintf(stderr, "Daemon already running (PID: %d)\n", pid);
                fclose(f);
                return 1;
            }
            fclose(f);
        }
        unlink(pp);
    }

    if (opts->daemon && !opts->foreground) {
        daemonize();
    }

    if (write_pid_file(pp) < 0) {
        fprintf(stderr, "Failed to write PID file\n");
        return 1;
    }

    atpd_context_init();
    atp_register_cleanup(&g_config);

    if (g_config.bypass_cn_ip && g_config.ebpf_enabled && g_config.cnip_mode == 1) {
        LOG_INFO("Initializing eBPF CNIP...");
        atpd_ebpf_state_transition(EBPF_STATE_LOADING);
        int ret = boxbpf_init_from_config(&g_config);
        if (ret == ATP_OK) {
            g_config.ebpf_ready = 1;
            g_atpd_ctx.ebpf_enabled = true;
            g_atpd_ctx.ebpf_probed = true;
            strncpy(g_atpd_ctx.ebpf_pin_dir, g_config.ebpf_pin_dir,
                    sizeof(g_atpd_ctx.ebpf_pin_dir) - 1);
            g_atpd_ctx.ebpf_pin_dir[sizeof(g_atpd_ctx.ebpf_pin_dir) - 1] = '\0';
            atpd_ebpf_state_transition(EBPF_STATE_READY);
            LOG_INFO("eBPF CNIP ready (pin: %s)", g_config.ebpf_pin_dir);
        } else {
            g_config.ebpf_ready = 0;
            g_atpd_ctx.ebpf_enabled = false;
            atpd_ebpf_state_transition(EBPF_STATE_FAILED);
            LOG_WARN("eBPF CNIP init failed, using ipset fallback");
        }
    } else {
        g_config.ebpf_ready = 0;
        g_atpd_ctx.ebpf_enabled = false;
        atpd_ebpf_state_transition(EBPF_STATE_DISABLED);
        if (!g_config.bypass_cn_ip) {
            LOG_DEBUG("CNIP bypass disabled");
        } else if (!g_config.ebpf_enabled) {
            LOG_DEBUG("eBPF disabled (ENABLE_EBPF=0)");
        } else if (g_config.cnip_mode != 1) {
            LOG_DEBUG("CNIP_MODE is not 'ebpf'");
        }
    }

    if (netlink_init(NULL, &g_config) < 0) {
        LOG_ERROR("Failed to initialize netlink");
        cleanup_ebpf();
        unlink(pp);
        return 1;
    }

    if (g_config.app_proxy_enable) {
        app_filter_init(&g_config);
    }

    fcm_monitor_init(&g_config);

    if (g_config.performance_mode) {
        perf_mode_init(&g_config);
    }

    api_init(&g_api_ctx, &g_config);

    g_svc = malloc(sizeof(service_ctx_t));
    if (!g_svc) {
        LOG_ERROR("Failed to allocate service context");
        cleanup_ebpf();
        unlink(pp);
        return 1;
    }

    if (service_init(g_svc, &g_config) < 0) {
        LOG_ERROR("Failed to initialize service");
        free(g_svc);
        g_svc = NULL;
        cleanup_ebpf();
        unlink(pp);
        return 1;
    }

    if (g_config.app_proxy_enable) {
        app_filter_setup(&g_config);
    }

    if (g_config.performance_mode) {
        perf_mode_setup(&g_config);
    }

    netlink_set_tproxy_ready();
    run_event_loop();

    return 0;
}

static int do_stop(atp_options_t *opts) {
    char cp[SAFE_PATH_MAX];
    char pp[SAFE_PATH_MAX];

    if (find_config_file(cp, SAFE_PATH_MAX, opts->config_file) == ATP_OK) {
        config_set_defaults(&g_config);
        config_load(cp, &g_config);
    } else {
        config_set_defaults(&g_config);
    }

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

    if (kill(pid, 0) < 0) {
        fprintf(stderr, "Daemon is not running (stale PID file)\n");
        atp_cleanup_manual(&g_config);
        unlink(pp);
        return 0;
    }

    if (!confirm_operation("stop", opts->force)) {
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
    kill(pid, SIGKILL);
    unlink(pp);
    boxbpf_clear();
    return 0;
}

static int do_status(atp_options_t *opts) {
    char cp[SAFE_PATH_MAX];
    if (find_config_file(cp, SAFE_PATH_MAX, opts->config_file) != ATP_OK) {
        fprintf(stderr, "Config file not found\n");
        return 1;
    }

    config_set_defaults(&g_config);
    config_load(cp, &g_config);

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
    char cp[SAFE_PATH_MAX];
    char pp[SAFE_PATH_MAX];

    if (find_config_file(cp, SAFE_PATH_MAX, opts->config_file) == ATP_OK) {
        config_set_defaults(&g_config);
        config_load(cp, &g_config);
    } else {
        config_set_defaults(&g_config);
    }

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

    if (kill(pid, SIGHUP) == 0) {
        printf("Reload signal sent to daemon (PID: %d)\n", pid);
        return 0;
    } else {
        fprintf(stderr, "Failed to send reload signal\n");
        return 1;
    }
}

static int do_check(atp_options_t *opts) {
    char cp[SAFE_PATH_MAX];
    if (find_config_file(cp, SAFE_PATH_MAX, opts->config_file) != ATP_OK) {
        fprintf(stderr, "Config file not found\n");
        return 1;
    }

    config_set_defaults(&g_config);
    if (config_load(cp, &g_config) != ATP_OK) {
        fprintf(stderr, "Failed to load config\n");
        return 1;
    }

    printf("Config file: %s\n", cp);
    printf("Configuration valid\n");
    return 0;
}

static int do_update_geoip(atp_options_t *opts) {
    (void)opts;

    if (!g_config.bypass_cn_ip) {
        printf("CNIP bypass disabled, skipping update\n");
        return 0;
    }

    printf("Updating GeoIP database...\n");

    int ret = geoip_force_update(&g_config);
    if (ret == 0) {
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
        printf("supported=1\n");
        printf("message=ok\n");
        printf("lpm_ipv4=1\n");
        printf("program_ipv4=1\n");
        printf("pin_ipv4=1\n");
        if (opts->ipv6) {
            printf("lpm_ipv6=1\n");
            printf("program_ipv6=1\n");
            printf("pin_ipv6=1\n");
        }
        return ATP_OK;
    } else {
        printf("supported=0\n");
        printf("message=eBPF xt_bpf unavailable\n");
        return ATP_ERR_EBPF;
    }
}

static int do_ebpf_init(atp_options_t *opts) {
    atp_config_t cfg;
    config_set_defaults(&cfg);
    if (opts->config_file[0] != '\0') {
        config_load(opts->config_file, &cfg);
    } else {
        char cp[SAFE_PATH_MAX];
        if (find_config_file(cp, SAFE_PATH_MAX, NULL) == ATP_OK) {
            config_load(cp, &cfg);
        }
    }
    if (opts->ebpf_config[0] != '\0') {
        strncpy(cfg.ebpf_config_path, opts->ebpf_config, sizeof(cfg.ebpf_config_path) - 1);
    }
    return boxbpf_init_from_config(&cfg);
}

static int do_ebpf_apply(atp_options_t *opts) {
    const char *config_path = opts->ebpf_config[0] ? opts->ebpf_config : "/data/adb/atp/ebpf/config.json";
    return boxbpf_apply(config_path);
}

static int do_ebpf_update(atp_options_t *opts) {
    const char *config_path = opts->ebpf_config[0] ? opts->ebpf_config : "/data/adb/atp/ebpf/config.json";
    return boxbpf_update(config_path);
}

static int do_ebpf_clear(atp_options_t *opts) {
    (void)opts;
    return boxbpf_clear();
}

static int do_ebpf_status(atp_options_t *opts) {
    char state[64] = {0};
    atp_config_t cfg;
    config_set_defaults(&cfg);
    if (opts->config_file[0] != '\0') {
        config_load(opts->config_file, &cfg);
    } else {
        char cp[SAFE_PATH_MAX];
        if (find_config_file(cp, SAFE_PATH_MAX, NULL) == ATP_OK) {
            config_load(cp, &cfg);
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

int main(int argc, char *argv[]) {
    atp_options_t opts = {0};

    if (parse_arguments(argc, argv, &opts) != 0) {
        return 1;
    }

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
