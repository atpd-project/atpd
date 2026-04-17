#include "atp.h"
#include "logger.h"
#include "config.h"
#include "tproxy.h"
#include "routing.h"
#include "netlink.h"
#include "service.h"
#include "geoip.h"
#include "api.h"
#include "cli.h"
#include "utils.h"
#include <libgen.h>
#include <sys/stat.h>

atp_config_t g_config;

static volatile sig_atomic_t g_running = 1;
static netlink_ctx_t g_netlink_ctx;
static service_ctx_t g_service_ctx;
static api_ctx_t g_api_ctx;

static void signal_handler(int sig) {
    if (sig == SIGTERM || sig == SIGINT) {
        LOG_INFO("Received signal %d, shutting down...", sig);
        g_running = 0;
    } else if (sig == SIGHUP) {
        LOG_INFO("Received SIGHUP, reloading configuration...");
        config_reload(&g_config);
    }
}

int atp_signal_setup(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    
    if (sigaction(SIGTERM, &sa, NULL) < 0) return -1;
    if (sigaction(SIGINT, &sa, NULL) < 0) return -1;
    if (sigaction(SIGHUP, &sa, NULL) < 0) return -1;
    
    signal(SIGPIPE, SIG_IGN);
    return 0;
}

int atp_check_root(void) {
    if (geteuid() != 0) {
        LOG_ERROR("Must run with root privileges (euid=%d)", geteuid());
        return -1;
    }
    LOG_DEBUG("Root privilege confirmed");
    return 0;
}

int atp_create_pidfile(void) {
    char pid_path[PATH_MAX];
    snprintf(pid_path, sizeof(pid_path), "%s/%s", 
             g_config.data_dir, ATP_PID_FILE);
    
    char *dir = dirname(strdup(pid_path));
    mkdir_recursive(dir, 0755);
    free(dir);
    
    FILE *fp = fopen(pid_path, "w");
    if (!fp) {
        LOG_ERROR("Failed to create PID file: %s", strerror(errno));
        return -1;
    }
    
    fprintf(fp, "%d\n", getpid());
    fclose(fp);
    LOG_DEBUG("PID file created: %s", pid_path);
    return 0;
}

void atp_remove_pidfile(void) {
    char pid_path[PATH_MAX];
    snprintf(pid_path, sizeof(pid_path), "%s/%s", 
             g_config.data_dir, ATP_PID_FILE);
    unlink(pid_path);
    LOG_DEBUG("PID file removed");
}

int atp_check_running(void) {
    char pid_path[PATH_MAX];
    snprintf(pid_path, sizeof(pid_path), "%s/%s", 
             g_config.data_dir, ATP_PID_FILE);
    
    FILE *fp = fopen(pid_path, "r");
    if (!fp) return 0;
    
    pid_t pid;
    fscanf(fp, "%d", &pid);
    fclose(fp);
    
    if (kill(pid, 0) == 0) {
        LOG_ERROR("ATP daemon already running with PID %d", pid);
        return 1;
    }
    
    unlink(pid_path);
    return 0;
}

void atp_daemonize(void) {
    if (g_config.foreground) {
        LOG_INFO("Running in foreground mode");
        return;
    }
    
    LOG_INFO("Daemonizing...");
    
    pid_t pid = fork();
    if (pid < 0) {
        LOG_FATAL("Fork failed: %s", strerror(errno));
        exit(1);
    }
    if (pid > 0) exit(0);
    
    setsid();
    
    pid = fork();
    if (pid < 0) {
        LOG_FATAL("Second fork failed: %s", strerror(errno));
        exit(1);
    }
    if (pid > 0) exit(0);
    
    chdir("/");
    
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
    
    open("/dev/null", O_RDONLY);
    open("/dev/null", O_WRONLY);
    open("/dev/null", O_WRONLY);
}

int atp_init(void) {
    char dirs[3][PATH_MAX];
    snprintf(dirs[0], sizeof(dirs[0]), "%s/run", g_config.data_dir);
    snprintf(dirs[1], sizeof(dirs[1]), "%s/rules", g_config.data_dir);
    snprintf(dirs[2], sizeof(dirs[2]), "%s/bin", g_config.data_dir);
    
    for (int i = 0; i < 3; i++) {
        mkdir_recursive(dirs[i], 0755);
    }
    
    char runtime_path[PATH_MAX];
    snprintf(runtime_path, sizeof(runtime_path), "%s/%s", 
             g_config.data_dir, ATP_RUNTIME_CONF);
    
    if (file_exists(runtime_path)) {
        config_load_runtime(runtime_path, &g_config);
    }
    
    return 0;
}

void atp_cleanup(void) {
    atp_remove_pidfile();
}

void atp_show_status(void) {
    printf("\n");
    LOG_INFO("==================== ATP Summary ====================");
    
    int pid = service_get_pid(&g_service_ctx);
    
    if (pid > 0) {
        long mem_kb = get_process_memory_kb(pid);
        double cpu = get_process_cpu_percent(pid);
        int threads = get_process_threads(pid);
        int fd_count = get_process_fd_count(pid);
        int uptime_sec = get_process_uptime_sec(pid);
        char uptime_str[64];
        char version[64];
        
        format_uptime(uptime_sec, uptime_str, sizeof(uptime_str));
        get_binary_version(PROXY_BIN_PATH, version, sizeof(version));
        
        char user[64], group[64];
        get_process_user_group(pid, user, group, sizeof(user));
        
        LOG_INFO("✓ Service Status: Running (PID: %d)", pid);
        LOG_INFO("    ├─ User:Group: %s:%s", user[0] ? user : "root", group[0] ? group : "net_admin");
        LOG_INFO("    ├─ Memory:     %.1f MB", mem_kb / 1024.0);
        LOG_INFO("    ├─ CPU:        %.1f%%", cpu);
        LOG_INFO("    ├─ Threads:    %d", threads);
        LOG_INFO("    ├─ Sockets:    %d (Active FDs)", fd_count);
        LOG_INFO("    ├─ Uptime:     %s", uptime_str);
        LOG_INFO("    └─ Version:    %s", version);
    } else {
        LOG_ERROR("✗ Service Status: Stopped/Failed");
        LOG_ERROR("Check logs in %s for core errors.", g_config.data_dir);
    }
    
    LOG_INFO("========================================================");
    printf("\n");
}

int main(int argc, char *argv[]) {
    atp_options_t opts;
    memset(&opts, 0, sizeof(opts));
    
    // Parse arguments first
    if (parse_arguments(argc, argv, &opts) != 0) {
        return 1;
    }
    
    // Handle help and version commands immediately (no root needed)
    if (opts.command == CMD_HELP) {
        print_help(argv[0]);
        return 0;
    }
    
    if (opts.command == CMD_VERSION) {
        print_version();
        return 0;
    }
    
    // Root check for all other commands
    if (atp_check_root() != 0) {
        return 1;
    }
    
    // Initialize logging
    log_init();
    if (opts.verbose) log_set_level(LOG_LEVEL_DEBUG);
    if (opts.quiet) log_set_level(LOG_LEVEL_ERROR);
    
    LOG_INFO(ATP_NAME " v" ATP_VERSION " starting");
    
    // Load configuration
    config_set_defaults(&g_config);
    
    if (opts.config_dir[0]) {
        strncpy(g_config.data_dir, opts.config_dir, sizeof(g_config.data_dir) - 1);
    }
    
    g_config.dry_run = opts.dry_run;
    g_config.verbose = opts.verbose;
    g_config.foreground = opts.foreground;
    
    char conf_path[PATH_MAX];
    snprintf(conf_path, sizeof(conf_path), "%s/%s", 
             g_config.data_dir, ATP_CONF_FILE);
    
    if (file_exists(conf_path)) {
        config_load(conf_path, &g_config);
    } else {
        LOG_WARN("Config file not found: %s, using defaults", conf_path);
    }
    
    // Handle status command (no daemonization needed)
    if (opts.command == CMD_STATUS) {
        service_init(&g_service_ctx, &g_config);
        atp_show_status();
        return 0;
    }
    
    // Handle update-geoip command
    if (opts.command == CMD_UPDATE_GEOIP) {
        geoip_init(&g_config);
        geoip_force_update(&g_config);
        return 0;
    }
    
    // Handle stop command
    if (opts.command == CMD_STOP) {
        service_init(&g_service_ctx, &g_config);
        service_stop(&g_service_ctx);
        tproxy_cleanup_all(&g_config);
        routing_cleanup_all(&g_config);
        LOG_INFO("ATP stopped");
        return 0;
    }
    
    // For start and restart, continue with daemon setup
    atp_signal_setup();
    
    if (atp_check_running()) return 1;
    
    atp_daemonize();
    atp_create_pidfile();
    atp_init();
    
    service_init(&g_service_ctx, &g_config);
    api_init(&g_api_ctx, &g_config);
    
    if (!service_check(&g_service_ctx)) {
        LOG_INFO("sing-box not running, starting...");
        service_start(&g_service_ctx);
    }
    
    tproxy_setup_ipv4(&g_config);
    routing_setup_ipv4(&g_config);
    
    if (g_config.proxy_ipv6) {
        tproxy_setup_ipv6(&g_config);
        routing_setup_ipv6(&g_config);
    }
    
    if (g_config.bypass_cn_ip) {
        geoip_download(&g_config);
        geoip_setup_ipset(&g_config);
    }
    
    if (g_config.block_quic) {
        tproxy_block_quic(&g_config, 1);
    }
    
    tproxy_block_loopback(&g_config, 1);
    
    netlink_init(&g_netlink_ctx);
    
    int heal_counter = 0;
    time_t last_api_sync = 0;
    
    while (g_running) {
        sleep(10);
        
        heal_counter++;
        if (heal_counter >= 3) {
            heal_counter = 0;
            char vpn_iface[IFNAMSIZ];
            if (netlink_get_active_vpn(&g_netlink_ctx, vpn_iface, sizeof(vpn_iface)) == 0) {
                if (!netlink_check_rule_exists(g_config.table_id, g_config.mark_value, vpn_iface)) {
                    LOG_WARN("Rule drift detected, repairing...");
                    routing_add_vpn_policy(&g_config, vpn_iface);
                    routing_add_mss_clamp(&g_config, vpn_iface);
                }
            }
        }
        
        if (!service_check(&g_service_ctx)) {
            LOG_ERROR("sing-box is offline!");
            if (!service_cooldown_active(&g_service_ctx)) {
                service_restart(&g_service_ctx);
            }
        }
        
        if (time(NULL) - last_api_sync > 300) {
            api_set_mode(&g_api_ctx, g_config.user_clash_mode);
            last_api_sync = time(NULL);
        }
    }
    
    netlink_cleanup(&g_netlink_ctx);
    service_stop(&g_service_ctx);
    tproxy_cleanup_all(&g_config);
    routing_cleanup_all(&g_config);
    
    atp_cleanup();
    LOG_INFO(ATP_NAME " stopped");
    
    return 0;
}
