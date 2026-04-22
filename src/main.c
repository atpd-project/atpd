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
static reactor_t *g_reactor = NULL;
static service_ctx_t *g_svc = NULL;
static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_reload = 0;
static volatile sig_atomic_t g_show_status = 0;

static void reactor_signal_cb(reactor_t *r, int sig, void *userdata) {
    (void)r;
    (void)userdata;
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

static void reactor_idle_cb(reactor_t *r, void *userdata) {
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

static void fcm_timer_cb(reactor_t *r, reactor_timer_t *timer, void *userdata) {
    (void)r;
    (void)timer;
    (void)userdata;
    fcm_monitor_poll();
}

static void service_timer_cb(reactor_t *r, reactor_timer_t *timer, void *userdata) {
    (void)r;
    (void)timer;
    (void)userdata;
    if (g_svc) {
        service_monitor(g_svc);
    }
}

static void netlink_io_cb(reactor_t *r, int fd, uint32_t events, void *userdata) {
    (void)r;
    (void)events;
    netlink_handle_event(fd, userdata);
}

static void run_event_loop(void) {
    g_reactor = reactor_create();
    if (!g_reactor) {
        LOG_ERROR("Failed to create reactor");
        return;
    }

    reactor_set_signal_cb(g_reactor, reactor_signal_cb);
    reactor_set_idle_cb(g_reactor, reactor_idle_cb);

    reactor_watch_signal(g_reactor, SIGINT);
    reactor_watch_signal(g_reactor, SIGTERM);
    reactor_watch_signal(g_reactor, SIGHUP);
    reactor_watch_signal(g_reactor, SIGUSR1);

    int nl_fd = netlink_get_fd();
    if (nl_fd >= 0) {
        reactor_add_fd(g_reactor, nl_fd, REACTOR_EVENT_READ, netlink_io_cb, NULL);
    }

    reactor_add_timer(g_reactor, 100, 100, fcm_timer_cb, NULL);
    reactor_add_timer(g_reactor, 500, 500, service_timer_cb, NULL);

    LOG_INFO("Reactor event loop started");
    reactor_run(g_reactor);
    LOG_INFO("Reactor event loop stopped");

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
    FILE *fp = fopen(pid_file, "w");
    if (!fp) return -1;
    fprintf(fp, "%d\n", getpid());
    fclose(fp);
    return 0;
}

static void get_binary_dir(char *buf, size_t size) {
    ssize_t len = readlink("/proc/self/exe", buf, size - 1);
    if (len != -1) {
        buf[len] = '\0';
        char *dir = dirname(buf);
        size_t dlen = strlen(dir);
        if (dlen > 0 && dlen < size) {
            memmove(buf, dir, dlen + 1);
        } else {
            buf[0] = '\0';
        }
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

static int do_start(atp_options_t *opts) {
    char cp[SAFE_PATH_MAX];
    char pp[SAFE_PATH_MAX];

    if (find_config_file(cp, SAFE_PATH_MAX, opts->config_file) != 0) {
        fprintf(stderr, "Config file not found\n");
        return 1;
    }

    config_set_defaults(&g_config);
    snprintf(g_config.data_dir, sizeof(g_config.data_dir), "%s", ATP_DEFAULT_DIR);

    if (snprintf(pp, sizeof(pp), "%s/%s", g_config.data_dir, ATP_PID_FILE) >= (int)sizeof(pp)) {
        return 1;
    }

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

    if (!opts->foreground && opts->daemon) {
        daemonize();
    }

    if (write_pid_file(pp) < 0) {
        fprintf(stderr, "Failed to write PID file\n");
        return 1;
    }

    g_config.foreground = opts->foreground;
    g_config.verbose = opts->verbose;
    config_load(cp, &g_config);

    logger_init();
    log_set_level(opts->log_level);
    if (opts->no_color) log_set_color(0);

    if (netlink_init(NULL, &g_config) < 0) {
        LOG_ERROR("Failed to initialize netlink");
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
        unlink(pp);
        return 1;
    }

    if (service_init(g_svc, &g_config) < 0) {
        LOG_ERROR("Failed to initialize service");
        free(g_svc);
        unlink(pp);
        return 1;
    }

    if (service_start(g_svc) < 0) {
        LOG_ERROR("Failed to start service");
        free(g_svc);
        unlink(pp);
        return 1;
    }

    if (g_config.app_proxy_enable) {
        app_filter_setup(&g_config);
    }

    if (g_config.performance_mode) {
        perf_mode_setup(&g_config);
    }

    run_event_loop();

    service_stop(g_svc);
    free(g_svc);
    g_svc = NULL;

    fcm_monitor_cleanup();
    netlink_cleanup();
    api_cleanup(&g_api_ctx);
    unlink(pp);
    logger_close();

    return 0;
}

static int do_stop(atp_options_t *opts) {
    char pp[SAFE_PATH_MAX];
    snprintf(pp, sizeof(pp), "%s/%s", ATP_DEFAULT_DIR, ATP_PID_FILE);

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
        unlink(pp);
        return 1;
    }

    if (!confirm_operation("stop", opts->force)) {
        return 0;
    }

    printf("Stopping daemon (PID: %d)...\n", pid);
    kill(pid, SIGTERM);

    for (int i = 0; i < 50; i++) {
        if (kill(pid, 0) < 0) {
            printf("Daemon stopped\n");
            unlink(pp);
            return 0;
        }
        usleep(100000);
    }

    printf("Daemon not responding, forcing kill...\n");
    kill(pid, SIGKILL);
    unlink(pp);
    return 0;
}

static int do_status(atp_options_t *opts) {
    char cp[SAFE_PATH_MAX];
    if (find_config_file(cp, SAFE_PATH_MAX, opts->config_file) != 0) {
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

    status_show(&g_config, &svc, &g_api_ctx);

    return 0;
}

static int do_reload(atp_options_t *opts) {
    (void)opts;
    char pp[SAFE_PATH_MAX];
    snprintf(pp, sizeof(pp), "%s/%s", ATP_DEFAULT_DIR, ATP_PID_FILE);

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

static int do_version(atp_options_t *opts) {
    (void)opts;
    printf("atpd v%s\n", ATP_VERSION_STRING);
    return 0;
}

static int do_help(atp_options_t *opts) {
    (void)opts;
    printf("Usage: atpd [start|stop|status|reload|version]\n");
    return 0;
}

typedef struct {
    const char *name;
    int (*handler)(atp_options_t *);
} command_t;

static const command_t commands[] = {
    {"start",   do_start},
    {"stop",    do_stop},
    {"status",  do_status},
    {"reload",  do_reload},
    {"version", do_version},
    {"help",    do_help},
    {NULL, NULL}
};

int main(int argc, char *argv[]) {
    atp_options_t opts = {0};

    static struct option long_options[] = {
        {"config",  required_argument, 0, 'c'},
        {"force",   no_argument,       0, 'f'},
        {"version", no_argument,       0, 'v'},
        {0, 0, 0, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "c:fv", long_options, NULL)) != -1) {
        if (c == 'c') {
            snprintf(opts.config_file, sizeof(opts.config_file), "%s", optarg);
        } else if (c == 'f') {
            opts.force = 1;
        } else if (c == 'v') {
            return do_version(&opts);
        }
    }

    if (optind >= argc) {
        return do_help(&opts);
    }

    for (int i = 0; commands[i].name; i++) {
        if (strcmp(commands[i].name, argv[optind]) == 0) {
            return commands[i].handler(&opts);
        }
    }

    fprintf(stderr, "Unknown command: %s\n", argv[optind]);
    return 1;
}
