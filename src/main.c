/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2025 ATP Project
 *
 * Main entry point for ATP daemon
 */

#include "atp.h"
#include "config.h"
#include "config_validator.h"
#include "logger.h"
#include "utils.h"
#include "service.h"
#include "api.h"
#include "netlink.h"
#include "netlink_monitor.h"
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
#include "epoll.h"
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

/* Global API context */
api_ctx_t g_api_ctx;

/* Global configuration */
atp_config_t g_config;

volatile sig_atomic_t g_running = 1;
volatile sig_atomic_t g_reload = 0;
volatile sig_atomic_t g_show_status = 0;

static void daemonize(void) {
    pid_t pid = fork();
    if (pid < 0) {
        LOG_ERROR("fork failed: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }
    
    if (setsid() < 0) {
        LOG_ERROR("setsid failed: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }
    
    pid = fork();
    if (pid < 0) {
        LOG_ERROR("second fork failed: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }
    
    umask(0);
    
    int fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > 2) close(fd);
    }
    
    chdir("/");
}

static int write_pid_file(const char *pid_file) {
    char dir[PATH_MAX];
    strncpy(dir, pid_file, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        mkdir_recursive(dir, 0755);
    }
    
    FILE *fp = fopen(pid_file, "w");
    if (!fp) {
        LOG_ERROR("Cannot write PID file: %s", pid_file);
        return -1;
    }
    fprintf(fp, "%d\n", getpid());
    fclose(fp);
    return 0;
}

static void remove_pid_file(const char *pid_file) {
    unlink(pid_file);
}

static void get_binary_dir(char *buf, size_t size) {
    ssize_t len = readlink("/proc/self/exe", buf, size - 1);
    if (len != -1) {
        buf[len] = '\0';
        char *dir = dirname(buf);
        strncpy(buf, dir, size - 1);
        buf[size - 1] = '\0';
    } else {
        buf[0] = '\0';
    }
}

static void get_default_config_path(char *path, size_t size) {
    char bin_dir[PATH_MAX];
    get_binary_dir(bin_dir, sizeof(bin_dir));
    if (bin_dir[0]) {
        snprintf(path, size, "%s/atp.conf", bin_dir);
    } else {
        snprintf(path, size, "./atp.conf");
    }
}

static int find_config_file(char *path, size_t size, const char *user_path) {
    if (user_path && user_path[0]) {
        strncpy(path, user_path, size - 1);
        path[size - 1] = '\0';
    } else {
        get_default_config_path(path, size);
    }
    
    if (access(path, R_OK) != 0) {
        return -1;
    }
    
    return 0;
}

static void print_config_error(const char *path) {
    fprintf(stderr, "atpd: Configuration file not found: %s\n", path);
    fprintf(stderr, "Try:\n");
    fprintf(stderr, "  - Place atp.conf in the same directory as atpd\n");
    fprintf(stderr, "  - Or use -c to specify config file: atpd -c /path/to/atp.conf <command>\n");
}

static int confirm_operation(const char *operation, int force) {
    if (force) {
        return 1;
    }
    
    char response[8];
    fprintf(stderr, "Warning: This will %s the ATP daemon", operation);
    if (strcmp(operation, "stop") == 0) {
        fprintf(stderr, " and interrupt proxy service");
    } else if (strcmp(operation, "restart") == 0) {
        fprintf(stderr, ", temporarily interrupting service");
    }
    fprintf(stderr, ".\nAre you sure? [y/N] ");
    
    if (fgets(response, sizeof(response), stdin) == NULL) {
        return 0;
    }
    
    return (response[0] == 'y' || response[0] == 'Y');
}

static void handle_netlink_fd(int fd, void *data) {
    (void)fd;
    (void)data;
    netlink_monitor_handle();
}

static void run_event_loop(service_ctx_t *svc, api_ctx_t *api) {
    int netlink_fd = netlink_monitor_get_fd();

    if (netlink_fd >= 0) {
        epoll_add_fd(netlink_fd, handle_netlink_fd, NULL);
    }

    LOG_INFO("Event loop started");

    while (g_running) {
        epoll_run_once(100);

        fcm_monitor_poll();

        if (svc) {
            service_monitor(svc);
        }

        if (g_reload) {
            LOG_INFO("Reloading configuration...");
            config_reload(&g_config);
            if (g_config.app_proxy_enable) {
                app_filter_reload(&g_config);
            }
            g_reload = 0;
        }

        if (g_show_status) {
            status_show(&g_config, svc, api);
            g_show_status = 0;
        }
    }

    LOG_INFO("Event loop stopped");
}

static int do_start(atp_options_t *opts) {
    char conf_path[PATH_MAX];
    char pid_path[PATH_MAX];
    
    if (find_config_file(conf_path, sizeof(conf_path), opts->config_file) != 0) {
        print_config_error(conf_path);
        return 1;
    }
    
    snprintf(pid_path, sizeof(pid_path), "%s/%s", g_config.data_dir, ATP_PID_FILE);
    
    if (file_exists(pid_path)) {
        FILE *fp = fopen(pid_path, "r");
        if (fp) {
            int pid;
            if (fscanf(fp, "%d", &pid) == 1) {
                if (kill(pid, 0) == 0) {
                    fprintf(stderr, "atpd: Daemon already running (PID: %d)\n", pid);
                    fclose(fp);
                    return 1;
                }
            }
            fclose(fp);
        }
        unlink(pid_path);
    }
    
    if (!opts->foreground && opts->daemon) {
        daemonize();
    }
    
    if (write_pid_file(pid_path) < 0) {
        return 1;
    }
    
    config_set_defaults(&g_config);
    strcpy(g_config.data_dir, ATP_DEFAULT_DIR);
    g_config.foreground = opts->foreground;
    g_config.verbose = opts->verbose;
    
    config_set_strict_mode(0);
    
    config_load(conf_path, &g_config);
    LOG_INFO("Configuration loaded from %s", conf_path);
    
    logger_init();
    log_set_level(opts->log_level);
    if (opts->no_color) {
        log_set_color(0);
    }
    
    LOG_INFO("ATP daemon starting (v" ATP_VERSION_STRING ")");
    
    if (epoll_init() < 0) {
        LOG_ERROR("Failed to initialize epoll");
        goto cleanup;
    }
    
    if (netlink_init() < 0) {
        LOG_ERROR("Failed to initialize netlink");
        goto cleanup;
    }
    
    if (g_config.app_proxy_enable) {
        if (app_filter_init(&g_config) < 0) {
            LOG_ERROR("Failed to initialize app filter");
            goto cleanup;
        }
    }
    
    if (netlink_monitor_init(&g_config) < 0) {
        LOG_ERROR("Failed to initialize netlink monitor");
        goto cleanup;
    }
    
    if (fcm_monitor_init(&g_config) < 0) {
        LOG_WARN("Failed to initialize FCM monitor");
    }
    
    if (g_config.performance_mode) {
        if (perf_mode_init(&g_config) < 0) {
            LOG_WARN("Failed to initialize performance mode");
        }
    }
    
    api_init(&g_api_ctx, &g_config);
    
    service_ctx_t *svc = malloc(sizeof(service_ctx_t));
    if (!svc) {
        LOG_ERROR("Failed to allocate service context");
        goto cleanup;
    }
    
    if (service_init(svc, &g_config) < 0) {
        LOG_ERROR("Failed to initialize service manager");
        free(svc);
        goto cleanup;
    }
    
    if (service_start(svc) < 0) {
        LOG_ERROR("Failed to start sing-box");
    }
    
    if (firewall_apply(&g_config) < 0) {
        LOG_ERROR("Failed to apply firewall rules");
    }
    
    if (g_config.app_proxy_enable) {
        if (app_filter_setup(&g_config) < 0) {
            LOG_WARN("Failed to setup app filter rules");
        }
    }
    
    if (g_config.performance_mode) {
        if (perf_mode_setup(&g_config) < 0) {
            LOG_WARN("Failed to setup performance mode");
        }
    }
    
    LOG_INFO("ATP daemon started successfully");
    
    run_event_loop(svc, &g_api_ctx);
    
    LOG_INFO("Shutting down...");
    
    firewall_cleanup(&g_config);
    
    if (g_config.app_proxy_enable) {
        app_filter_cleanup(&g_config);
    }
    
    if (g_config.performance_mode) {
        perf_mode_cleanup(&g_config);
    }
    
    service_stop(svc);
    free(svc);
    
    fcm_monitor_cleanup();
    netlink_monitor_cleanup();
    netlink_cleanup();
    
cleanup:
    epoll_cleanup();
    remove_pid_file(pid_path);
    logger_close();
    
    return 0;
}

static int do_stop(atp_options_t *opts) {
    char conf_path[PATH_MAX];
    char pid_path[PATH_MAX];
    int config_loaded = 0;

    if (opts->config_file[0]) {
        if (access(opts->config_file, R_OK) == 0) {
            config_set_defaults(&g_config);
            config_load(opts->config_file, &g_config);
            config_loaded = 1;
        }
    } else {
        get_default_config_path(conf_path, sizeof(conf_path));
        if (access(conf_path, R_OK) == 0) {
            config_set_defaults(&g_config);
            config_load(conf_path, &g_config);
            config_loaded = 1;
        }
    }

    if (config_loaded) {
        snprintf(pid_path, sizeof(pid_path), "%s/%s", g_config.data_dir, ATP_PID_FILE);
    } else {
        snprintf(pid_path, sizeof(pid_path), "%s/%s", ATP_DEFAULT_DIR, ATP_PID_FILE);
    }

    FILE *fp = fopen(pid_path, "r");
    if (!fp) {
        fprintf(stderr, "atpd: Daemon is not running (no PID file)\n");
        return 1;
    }

    int pid;
    if (fscanf(fp, "%d", &pid) != 1) {
        fprintf(stderr, "atpd: Invalid PID file\n");
        fclose(fp);
        return 1;
    }
    fclose(fp);

    if (kill(pid, 0) < 0) {
        fprintf(stderr, "atpd: Daemon is not running (stale PID file)\n");
        unlink(pid_path);
        return 1;
    }

    if (!confirm_operation("stop", opts->force)) {
        fprintf(stderr, "Aborted.\n");
        return 0;
    }

    printf("Stopping daemon (PID: %d)...\n", pid);
    kill(pid, SIGTERM);

    for (int i = 0; i < 50; i++) {
        if (kill(pid, 0) < 0) {
            printf("Daemon stopped\n");
            unlink(pid_path);
            return 0;
        }
        usleep(100000);
    }

    printf("Daemon not responding, forcing kill...\n");
    kill(pid, SIGKILL);
    unlink(pid_path);

    return 0;
}

static int do_restart(atp_options_t *opts) {
    if (!confirm_operation("restart", opts->force)) {
        fprintf(stderr, "Aborted.\n");
        return 0;
    }

    int ret = do_stop(opts);
    if (ret != 0) {
        return ret;
    }

    sleep(2);

    return do_start(opts);
}

static int do_status(atp_options_t *opts) {
    char conf_path[PATH_MAX];

    if (find_config_file(conf_path, sizeof(conf_path), opts->config_file) != 0) {
        print_config_error(conf_path);
        return 1;
    }

    config_set_defaults(&g_config);
    config_load(conf_path, &g_config);

    if (opts->no_color) {
        ui_set_no_color(1);
    }
    ui_init();

    char pid_path[PATH_MAX];
    snprintf(pid_path, sizeof(pid_path), "%s/%s", g_config.data_dir, ATP_PID_FILE);

    service_ctx_t svc;
    memset(&svc, 0, sizeof(svc));
    service_init(&svc, &g_config);

    api_init(&g_api_ctx, &g_config);

    status_show(&g_config, &svc, &g_api_ctx);

    return 0;
}

static int do_reload(atp_options_t *opts) {
    char conf_path[PATH_MAX];
    char pid_path[PATH_MAX];

    if (opts->config_file[0]) {
        if (access(opts->config_file, R_OK) != 0) {
            fprintf(stderr, "atpd: %s: No such file or directory\n", opts->config_file);
            return 1;
        }
        config_set_defaults(&g_config);
        config_load(opts->config_file, &g_config);
    } else {
        get_default_config_path(conf_path, sizeof(conf_path));
        if (access(conf_path, R_OK) != 0) {
            print_config_error(conf_path);
            return 1;
        }
        config_set_defaults(&g_config);
        config_load(conf_path, &g_config);
    }

    snprintf(pid_path, sizeof(pid_path), "%s/%s", g_config.data_dir, ATP_PID_FILE);

    FILE *fp = fopen(pid_path, "r");
    if (!fp) {
        fprintf(stderr, "atpd: Daemon is not running\n");
        return 1;
    }

    int pid;
    if (fscanf(fp, "%d", &pid) != 1) {
        fprintf(stderr, "atpd: Invalid PID file\n");
        fclose(fp);
        return 1;
    }
    fclose(fp);

    if (kill(pid, SIGHUP) < 0) {
        fprintf(stderr, "atpd: Failed to reload daemon: %s\n", strerror(errno));
        return 1;
    }

    printf("Reload signal sent to daemon (PID: %d)\n", pid);
    return 0;
}

static int do_check(atp_options_t *opts) {
    char conf_path[PATH_MAX];

    if (find_config_file(conf_path, sizeof(conf_path), opts->config_file) != 0) {
        fprintf(stderr, "atpd: %s: No such file or directory\n", conf_path);
        return 1;
    }

    printf("Configuration file: %s\n", conf_path);

    config_set_defaults(&g_config);
    config_set_strict_mode(1);

    if (config_load(conf_path, &g_config) != 0) {
        fprintf(stderr, "Syntax check failed.\n");
        return 2;
    }

    printf("Syntax check passed.\n");

    int validation_result = config_validate_values(&g_config);

    if (validation_result != 0) {
        fprintf(stderr, "Configuration validation failed with errors.\n");
        return 3;
    }

    printf("Configuration validation passed.\n");
    return 0;
}

static int do_update_geoip(atp_options_t *opts) {
    char conf_path[PATH_MAX];

    if (find_config_file(conf_path, sizeof(conf_path), opts->config_file) != 0) {
        print_config_error(conf_path);
        return 1;
    }

    config_set_defaults(&g_config);
    config_load(conf_path, &g_config);

    logger_init();
    log_set_level(opts->log_level);

    printf("Configuration loaded from %s\n", conf_path);
    printf("Updating GeoIP database...\n");

    geoip_init(&g_config);

    int ret = geoip_force_update(&g_config);

    if (ret == 0) {
        printf("GeoIP database updated successfully.\n");
    } else {
        fprintf(stderr, "Failed to update GeoIP database.\n");
    }

    return ret;
}

int main(int argc, char *argv[]) {
    atp_options_t opts;

    if (parse_arguments(argc, argv, &opts) != 0) {
        return 1;
    }

    if (opts.no_color) {
        log_set_color(0);
    }

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
        case CMD_UPDATE_GEOIP:
            return do_update_geoip(&opts);
        case CMD_VERSION:
            print_version();
            return 0;
        case CMD_HELP:
            print_help(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 0;
    }
}
