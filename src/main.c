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
#include "cli.h"
#include "version.h"
#include "reactor.h"
#include "uds.h"
#include "routing.h"
#include "wifi.h"
#include "yyjson.h"

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

static int run_event_loop(void);
static void on_signal(reactor_t *r, int sig, void *userdata);
static void on_idle(reactor_t *r, void *userdata);
static int write_pid_file(const char *pid_file);
static void resolve_pid_path(atp_options_t *opts, char *pp, size_t size);
static void daemonize(void);
static int process_is_atpd(pid_t pid);
static int verify_pid_file_unchanged(int fd, int expected_pid);
static void network_state_cb(nl_event_type_t event, const char *iface, void *userdata);
static void request_clash_mode(const char *mode);
static void reconcile_clash_mode(void);
static void refresh_direct_wifi_state(void);

static int g_pid_fd = -1;
static int g_mode_request_inflight = 0;
static int g_version_request_inflight = 0;
static pid_t g_mode_pid = -1;
static char g_desired_mode[32] = "";
static char g_applied_mode[32] = "";
static char g_requested_mode[32] = "";

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

    ssize_t len = -1;
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

    return strcmp(base, "atpd") == 0 ||
           strcmp(base, "atpd-zig") == 0 ||
           strcmp(base, "atpd.bin") == 0;
}

static int verify_pid_file_unchanged(int fd, int expected_pid) {
    char buf[32] = {0};
    ssize_t n = pread(fd, buf, sizeof(buf) - 1, 0);
    int current_pid = 0;

    if (n > 0 && sscanf(buf, "%d", &current_pid) == 1) {
        return (current_pid == expected_pid) ? 0 : -1;
    }
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

    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        for (int fd = 0; fd < (int)rl.rlim_cur; fd++) {
            if (fd > 2) {
                close(fd);
            }
        }
    }

    int fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > 2) close(fd);
    }

    signal(SIGPIPE, SIG_IGN);
    if (chdir("/") != 0) {
        fprintf(stderr, "Error: chdir failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
}

static void on_signal(reactor_t *r, int sig, void *userdata) {
    (void)userdata;
    if (sig == SIGCHLD) {
        service_sigchld_cb(r, sig, g_svc);
    } else if (sig == SIGHUP) {
        g_reload = 1;
    } else if (sig == SIGUSR1) {
        g_show_status = 1;
    } else {
        g_running = 0;
        reactor_stop(r);
    }
}

static void on_idle(reactor_t *r, void *userdata) {
    (void)r;
    (void)userdata;

    if (uds_stop_requested()) g_running = 0;
    if (uds_reload_requested()) g_reload = 1;
    uds_clear_requests();

    if (g_reload) {
        g_reload = 0;
        atpd_runtime_state_transition(ATPD_RUNTIME_STATE_RELOADING);
        LOG_INFO("Processing config reload...");
        if (config_reload(&g_config) != ATP_OK) {
            LOG_ERROR("Config reload failed");
            atpd_runtime_state_transition(ATPD_RUNTIME_STATE_FAILED);
        } else {
            api_cleanup(&g_api_ctx);
            g_mode_request_inflight = 0;
            g_version_request_inflight = 0;
            g_mode_pid = -1;
            if (api_init(&g_api_ctx, &g_config) != 0 ||
                api_start_with_reactor(&g_api_ctx, g_reactor) != 0) {
                LOG_ERROR("Clash API reload failed");
                atpd_runtime_state_transition(ATPD_RUNTIME_STATE_FAILED);
            } else if (service_reload_async(g_svc, &g_config) != 0) {
                LOG_ERROR("sing-box reload failed validation");
                atpd_runtime_state_transition(ATPD_RUNTIME_STATE_FAILED);
            } else {
                netlink_refresh_now();
                refresh_direct_wifi_state();
                LOG_INFO("Config reload scheduled successfully");
                atpd_runtime_state_transition(ATPD_RUNTIME_STATE_RUNNING);
            }
        }
    }

    if (g_show_status) {
        g_show_status = 0;
        LOG_INFO("Status: backend=%s sing-box=%s pid=%d vpn=%s",
                 g_config.network.backend,
                 g_svc ? service_state_string(g_svc->state) : "UNKNOWN",
                 g_svc ? service_get_pid(g_svc) : -1,
                 g_atpd_ctx.vpn_iface[0] ? g_atpd_ctx.vpn_iface : "none");
    }

    if (!g_running) {
        LOG_INFO("Stopping reactor...");
        atpd_runtime_state_transition(ATPD_RUNTIME_STATE_STOPPED);
        reactor_stop(r);
    }
}

static void clash_mode_response(int http_code, const char *body, void *userdata) {
    (void)body;
    (void)userdata;
    g_mode_request_inflight = 0;
    if (http_code >= 200 && http_code < 300) {
        g_mode_pid = g_svc ? service_get_pid(g_svc) : -1;
        snprintf(g_applied_mode, sizeof(g_applied_mode), "%s", g_requested_mode);
        snprintf(g_atpd_ctx.clash_applied_mode,
                 sizeof(g_atpd_ctx.clash_applied_mode), "%s", g_requested_mode);
        g_atpd_ctx.clash_last_error[0] = '\0';
        g_atpd_ctx.clash_last_sync = (uint64_t)time(NULL);
        LOG_INFO("Clash mode synchronized: %s", g_requested_mode);
    } else {
        LOG_WARN("Clash mode synchronization failed: HTTP %d", http_code);
        snprintf(g_atpd_ctx.clash_last_error,
                 sizeof(g_atpd_ctx.clash_last_error), "HTTP %d", http_code);
    }
    if (http_code >= 200 && http_code < 300 &&
        strcmp(g_applied_mode, g_desired_mode) != 0) {
        request_clash_mode(g_desired_mode);
    }
}

static void request_clash_mode(const char *mode) {
    pid_t core_pid = g_svc ? service_get_pid(g_svc) : -1;
    if (core_pid > 0 && core_pid != g_mode_pid) {
        g_applied_mode[0] = '\0';
        g_atpd_ctx.clash_applied_mode[0] = '\0';
    }
    snprintf(g_desired_mode, sizeof(g_desired_mode), "%s", mode);
    snprintf(g_atpd_ctx.clash_desired_mode,
             sizeof(g_atpd_ctx.clash_desired_mode), "%s", mode);
    if (g_mode_request_inflight || strcmp(g_applied_mode, mode) == 0) return;

    snprintf(g_requested_mode, sizeof(g_requested_mode), "%s", mode);
    g_mode_request_inflight = 1;
    if (api_set_mode_async(&g_api_ctx, mode, clash_mode_response, NULL) != 0) {
        g_mode_request_inflight = 0;
    }
}

static void singbox_version_response(int http_code, const char *body, void *userdata) {
    (void)userdata;
    g_version_request_inflight = 0;
    if (!g_svc || http_code < 200 || http_code >= 300 || !body) return;

    yyjson_doc *doc = yyjson_read(body, strlen(body), 0);
    if (!doc) return;
    const char *version = yyjson_get_str(
        yyjson_obj_get(yyjson_doc_get_root(doc), "version"));
    if (version && version[0]) {
        snprintf(g_svc->version, sizeof(g_svc->version), "%s", version);
    }
    yyjson_doc_free(doc);
}

static void network_state_cb(nl_event_type_t event, const char *iface, void *userdata) {
    (void)userdata;
    if (event == NL_EVENT_VPN_CONNECTED && iface && iface[0]) {
        routing_add_vpn_policy(&g_config, iface);
    } else if (event == NL_EVENT_VPN_DISCONNECTED) {
        routing_remove_vpn_policy(&g_config, NULL);
    }
    reconcile_clash_mode();
}

static void reconcile_clash_mode(void) {
    vpn_state_t vpn_state = atomic_load(&g_atpd_ctx.vpn_state);
    direct_wifi_state_t wifi_state = atomic_load(&g_atpd_ctx.direct_wifi_state);
    const char *base = atpd_clash_target_mode(
        vpn_state, DIRECT_WIFI_DISCONNECTED, g_config.filter.user_clash_mode);
    if (wifi_state == DIRECT_WIFI_ACTIVE) {
        snprintf(g_atpd_ctx.direct_wifi_restore_mode,
                 sizeof(g_atpd_ctx.direct_wifi_restore_mode), "%s", base);
    }
    request_clash_mode(atpd_clash_target_mode(vpn_state, wifi_state, base));
}

static void refresh_direct_wifi_state(void) {
    if (!g_config.filter.direct_wifi_ssid[0]) {
        atpd_direct_wifi_state_transition(DIRECT_WIFI_DISABLED, NULL);
        reconcile_clash_mode();
        return;
    }

    char ssid[sizeof(g_atpd_ctx.current_wifi_ssid)];
    int result = wifi_get_ssid(ssid, sizeof(ssid));
    if (result < 0) {
        LOG_DEBUG("Wi-Fi status unavailable; keeping previous Direct state");
        return;
    }

    direct_wifi_state_t state = result == 0 &&
        strcmp(ssid, g_config.filter.direct_wifi_ssid) == 0
        ? DIRECT_WIFI_ACTIVE : DIRECT_WIFI_DISCONNECTED;
    atpd_direct_wifi_state_transition(state, result == 0 ? ssid : NULL);
    reconcile_clash_mode();
}

static int fcm_connection_active_in(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;

    char line[512];
    int found = 0;
    while (fgets(line, sizeof(line), fp)) {
        unsigned remote_port;
        unsigned state;
        if (sscanf(line, " %*d: %*s %*64[0-9A-Fa-f]:%x %x",
                   &remote_port, &state) == 2 && remote_port == 5228 && state == 1) {
            found = 1;
            break;
        }
    }
    fclose(fp);
    return found;
}

static int fcm_connection_active(void) {
    /* ponytail: port-only detection avoids the old DNS/thread subsystem. */
    return fcm_connection_active_in("/proc/net/tcp") ||
           fcm_connection_active_in("/proc/net/tcp6");
}

static void network_audit_cb(reactor_t *r, reactor_timer_t *timer, void *userdata) {
    (void)r;
    (void)timer;
    (void)userdata;
    netlink_refresh_now();
    if (g_svc && g_svc->state == SERVICE_RUNNING && !g_svc->version[0] &&
        !g_version_request_inflight) {
        g_version_request_inflight = 1;
        if (api_get_version_async(&g_api_ctx, singbox_version_response, NULL) != 0) {
            g_version_request_inflight = 0;
        }
    }
}

static void mode_monitor_cb(reactor_t *r, reactor_timer_t *timer, void *userdata) {
    (void)r;
    (void)timer;
    (void)userdata;
    refresh_direct_wifi_state();
    vpn_state_t vpn_state = atomic_load(&g_atpd_ctx.vpn_state);
    g_atpd_ctx.fcm_monitor_active = vpn_state == VPN_STATE_PREDICTING ||
                                    vpn_state == VPN_STATE_READY ||
                                    vpn_state == VPN_STATE_TEARDOWN;
    if (g_atpd_ctx.fcm_monitor_active && fcm_connection_active()) {
        g_atpd_ctx.fcm_last_seen = (uint64_t)time(NULL);
    }
}

static void netlink_io_cb(reactor_t *r, int fd, uint32_t events, void *userdata) {
    (void)r;
    (void)events;
    netlink_handle_event(fd, userdata);
}

static int run_event_loop(void) {
    int result = -1;
    reactor_timer_t *network_timer = NULL;
    reactor_timer_t *mode_timer = NULL;
    if (!g_reactor) {
        LOG_ERROR("Reactor is not initialized");
        return -1;
    }

    char uds_path[SAFE_PATH_MAX];
    snprintf(uds_path, sizeof(uds_path), "%s/run/atpd.sock", g_config.core.data_dir);
    if (uds_init(g_reactor, uds_path) != 0) goto cleanup;

    if (g_svc && service_start_async(g_svc) != 0) {
        LOG_ERROR("Failed to start service");
        goto cleanup;
    }

    network_timer = reactor_add_timer(g_reactor, 1000, 10000,
                                      network_audit_cb, NULL);
    mode_timer = reactor_add_timer(g_reactor, 1000, 5000,
                                   mode_monitor_cb, NULL);
    if (!network_timer || !mode_timer) {
        LOG_ERROR("Failed to schedule network monitors");
        goto cleanup;
    }

    if (reactor_set_signal_cb(g_reactor, on_signal) != 0) goto cleanup;
    reactor_set_idle_cb(g_reactor, on_idle);

    const int signals[] = {SIGINT, SIGTERM, SIGHUP, SIGUSR1, SIGCHLD};
    for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
        if (reactor_watch_signal(g_reactor, signals[i]) != 0) {
            LOG_ERROR("Failed to watch signal %d", signals[i]);
            goto cleanup;
        }
    }

    int nl_fd = netlink_get_fd();
    if (nl_fd >= 0) {
        if (reactor_add_fd(g_reactor, nl_fd, REACTOR_EVENT_READ,
                           netlink_io_cb, NULL) != 0) {
            LOG_ERROR("Failed to register netlink event socket");
            goto cleanup;
        }
    } else {
        LOG_ERROR("Netlink event socket is unavailable");
        goto cleanup;
    }

    LOG_INFO("Reactor event loop started");
    result = reactor_run(g_reactor);
    LOG_INFO("Reactor event loop stopped");

cleanup:
    if (network_timer) reactor_cancel_timer(g_reactor, network_timer);
    if (mode_timer) reactor_cancel_timer(g_reactor, mode_timer);

    routing_remove_vpn_policy(&g_config, g_config.interface.current_vpn_iface);
    service_ctx_t *svc = g_svc;
    g_svc = NULL;
    if (svc) {
        service_cleanup(svc);
        free(svc);
    }

    netlink_cleanup();
    uds_cleanup();
    api_cleanup(&g_api_ctx);

    if (g_reactor) {
        reactor_destroy(g_reactor);
        g_reactor = NULL;
    }
    return result;
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
        .opts = opts,
        .netlink_callback = network_state_cb,
        .netlink_userdata = NULL
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
    atpd_runtime_state_transition(ATPD_RUNTIME_STATE_INITIALIZING);
    g_reactor = reactor_create();
    if (!g_reactor) {
        ret = 1;
        goto cleanup;
    }
    init_ctx.reactor = g_reactor;

    if (atpd_init_run(&init_ctx) != 0) {
        LOG_ERROR("Initialization failed");
        atpd_runtime_state_transition(ATPD_RUNTIME_STATE_FAILED);
        ret = 1;
        goto cleanup;
    }
    g_svc = init_ctx.service;
    snprintf(g_desired_mode, sizeof(g_desired_mode), "%s",
             g_config.filter.user_clash_mode);
    snprintf(g_atpd_ctx.clash_desired_mode,
             sizeof(g_atpd_ctx.clash_desired_mode), "%s", g_desired_mode);
    atpd_runtime_state_transition(ATPD_RUNTIME_STATE_RUNNING);

    ret = run_event_loop() == 0 ? 0 : 1;

cleanup:
    if (ret != 0) {
        routing_remove_vpn_policy(&g_config, NULL);
        netlink_cleanup();
        uds_cleanup();
        api_cleanup(&g_api_ctx);
        if (g_svc) {
            service_cleanup(g_svc);
            free(g_svc);
            g_svc = NULL;
        }
        if (g_reactor) {
            reactor_destroy(g_reactor);
            g_reactor = NULL;
        }
    }

cleanup_return:
    if (g_pid_fd >= 0) {
        close(g_pid_fd);
        g_pid_fd = -1;
    }
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

    char buf[32] = {0};
    ssize_t n = pread(pid_file_fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
        close(pid_file_fd);
        fprintf(stderr, "Invalid PID file\n");
        return 1;
    }

    int pid;
    if (sscanf(buf, "%d", &pid) != 1) {
        close(pid_file_fd);
        fprintf(stderr, "Invalid PID format\n");
        return 1;
    }

    if (!process_is_atpd(pid)) {
        fprintf(stderr, "Process %d is not ATP daemon\n", pid);
        close(pid_file_fd);
        unlink(pp);
        return 1;
    }

    if (kill(pid, 0) < 0) {
        fprintf(stderr, "Daemon is not running (stale PID file)\n");
        close(pid_file_fd);
        unlink(pp);
        return 0;
    }

    printf("Stopping daemon (PID: %d)...\n", pid);

    if (verify_pid_file_unchanged(pid_file_fd, pid) < 0) {
        fprintf(stderr, "PID file changed during operation\n");
        close(pid_file_fd);
        return 1;
    }

    close(pid_file_fd);

    if (kill(pid, SIGTERM) < 0 && errno != ESRCH) {
        fprintf(stderr, "Failed to send SIGTERM: %s\n", strerror(errno));
        return 1;
    }

    for (int i = 0; i < SERVICE_STOP_RETRY_COUNT; i++) {
        if (kill(pid, 0) < 0) {
            printf("Daemon stopped\n");
            unlink(pp);
            return 0;
        }
        usleep(SERVICE_STOP_INTERVAL_MS * 1000);
    }

    printf("Daemon not responding, forcing kill...\n");
    int retry_count = SIGNAL_RETRY_MAX;
    while (retry_count-- > 0) {
        if (kill(pid, SIGKILL) == 0) {
            ret = 0;
            break;
        }
        if (errno == ESRCH) {
            ret = 0;
            break;
        }
        if (errno != EINTR) {
            ret = 1;
            break;
        }
        usleep(SIGNAL_RETRY_DELAY_US);
    }

    unlink(pp);
    return ret;
}

static int do_restart(atp_options_t *opts) {
    LOG_INFO("Restarting daemon...");
    do_stop(opts);
    sleep(2);
    return do_start(opts);
}

static int do_status(atp_options_t *opts) {
    const char *config_path = opts->config_file[0]
        ? opts->config_file : ATP_DEFAULT_DIR "/" ATP_CONF_FILE;
    log_set_level(LOG_LEVEL_NONE);
    if (access(config_path, R_OK) == 0) config_load(config_path, &g_config);

    char socket_path[SAFE_PATH_MAX];
    snprintf(socket_path, sizeof(socket_path), "%s/run/atpd.sock",
             g_config.core.data_dir);
    int status = uds_client_status(socket_path, stdout);
    if (status >= 0) return status;

    char pid_path[SAFE_PATH_MAX];
    snprintf(pid_path, sizeof(pid_path), "%s/%s", g_config.core.data_dir, ATP_PID_FILE);
    const char *pid_state = access(pid_path, F_OK) == 0 ? "stale or unavailable" : "missing";
    printf("ATPd %s                                      STOPPED\n\n", ATP_VERSION_STRING);
    printf("Daemon\n");
    printf("  State               STOPPED\n");
    printf("  Config              %s\n", config_path);
    printf("  PID file            %s (%s)\n\n", pid_path, pid_state);
    printf("Overall               STOPPED\n");
    return 2;
}

static int do_core_status(atp_options_t *opts) {
    const char *config_path = opts->config_file[0]
        ? opts->config_file : ATP_DEFAULT_DIR "/" ATP_CONF_FILE;
    log_set_level(LOG_LEVEL_NONE);
    if (config_load(config_path, &g_config) != ATP_OK) {
        fprintf(stderr, "Failed to load config: %s\n", config_path);
        return 1;
    }

    char socket_path[SAFE_PATH_MAX];
    snprintf(socket_path, sizeof(socket_path), "%s/run/atpd.sock",
             g_config.core.data_dir);
    int status = uds_client_core_status(socket_path, stdout);
    if (status >= 0) return status;

    api_ctx_t api;
    api_init(&api, &g_config);
    char version[64] = "unknown";
    char mode[32] = "unknown";
    int version_ok = api_get_version_sync(&api, version, sizeof(version)) == 0;
    int mode_ok = api_get_mode_sync(&api, mode, sizeof(mode)) == 0;
    api_cleanup(&api);

    const char *overall = version_ok && mode_ok ? "HEALTHY" :
        version_ok || mode_ok ? "DEGRADED" : "UNREACHABLE";
    printf("sing-box\n");
    printf("  API                 %s\n", version_ok || mode_ok ? "REACHABLE" : "UNREACHABLE");
    printf("  Endpoint            %s:%d\n", g_config.api.host, g_config.api.port);
    printf("  Version             %s\n", version);
    printf("  Mode                %s\n\n", mode);
    printf("Overall               %s\n", overall);
    return version_ok && mode_ok ? 0 : version_ok || mode_ok ? 1 : 2;
}

static int do_core_control(atp_options_t *opts, const char *action) {
    const char *config_path = opts->config_file[0]
        ? opts->config_file : ATP_DEFAULT_DIR "/" ATP_CONF_FILE;
    log_set_level(LOG_LEVEL_NONE);
    if (config_load(config_path, &g_config) != ATP_OK) {
        fprintf(stderr, "Failed to load config: %s\n", config_path);
        return 1;
    }

    char socket_path[SAFE_PATH_MAX];
    char command[32];
    snprintf(socket_path, sizeof(socket_path), "%s/run/atpd.sock",
             g_config.core.data_dir);
    snprintf(command, sizeof(command), "core %s", action);
    int status = uds_client_core_control(socket_path, command, stdout);
    if (status < 0) {
        fprintf(stderr, "ATPd is unavailable; cannot %s core\n", action);
        return 1;
    }
    return status;
}

static int do_reload(atp_options_t *opts) {
    char pp[SAFE_PATH_MAX];
    int pid_file_fd = -1;

    resolve_pid_path(opts, pp, sizeof(pp));

    pid_file_fd = open(pp, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (pid_file_fd < 0) {
        fprintf(stderr, "Daemon is not running\n");
        return 1;
    }

    char buf[32] = {0};
    ssize_t n = pread(pid_file_fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
        close(pid_file_fd);
        fprintf(stderr, "Invalid PID file\n");
        return 1;
    }

    int pid;
    if (sscanf(buf, "%d", &pid) != 1) {
        close(pid_file_fd);
        fprintf(stderr, "Invalid PID format\n");
        return 1;
    }

    if (!process_is_atpd(pid)) {
        fprintf(stderr, "Process %d is not ATP daemon\n", pid);
        close(pid_file_fd);
        return 1;
    }

    if (kill(pid, 0) < 0) {
        fprintf(stderr, "Daemon is not running\n");
        close(pid_file_fd);
        return 1;
    }

    if (verify_pid_file_unchanged(pid_file_fd, pid) < 0) {
        fprintf(stderr, "PID file changed during operation\n");
        close(pid_file_fd);
        return 1;
    }

    close(pid_file_fd);

    if (kill(pid, SIGHUP) == 0) {
        printf("Reload signal sent to daemon (PID: %d)\n", pid);
        return 0;
    } else {
        fprintf(stderr, "Failed to send reload signal: %s\n", strerror(errno));
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

    if (config_load(config_path, &g_config) != ATP_OK) {
        fprintf(stderr, "Failed to load config\n");
        return 1;
    }

    service_ctx_t svc;
    service_init(&svc, &g_config);
    if (service_validate_config(svc.conf_path) != 0) {
        fprintf(stderr, "Invalid sing-box eBPF configuration: %s\n", svc.conf_path);
        return 1;
    }

    printf("Config file: %s\n", config_path);
    printf("sing-box config: %s\n", svc.conf_path);
    printf("Configuration valid\n");
    return 0;
}

int main(int argc, char *argv[]) {
    atp_options_t opts = {0};

    if (parse_arguments(argc, argv, &opts) != 0) {
        return 1;
    }

    config_set_defaults(&g_config);

    switch (opts.command) {
        case CMD_START:
            return do_start(&opts);
        case CMD_STOP:
            return do_stop(&opts);
        case CMD_RESTART:
            return do_restart(&opts);
        case CMD_STATUS:
            return do_status(&opts);
        case CMD_CORE_STATUS:
            return do_core_status(&opts);
        case CMD_CORE_START:
            return do_core_control(&opts, "start");
        case CMD_CORE_STOP:
            return do_core_control(&opts, "stop");
        case CMD_CORE_RESTART:
            return do_core_control(&opts, "restart");
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
        default:
            print_usage(argv[0]);
            return 0;
    }
}
