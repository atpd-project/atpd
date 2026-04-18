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
#include "status.h"
#include "app_filter.h"
#include "mac_filter.h"
#include "ipv6_manager.h"
#include "perf_mode.h"
#include "utils.h"
#include <libgen.h>
#include <sys/stat.h>
#include <sys/wait.h>

atp_config_t g_config;

static volatile sig_atomic_t g_running = 1;
static service_ctx_t g_service_ctx;
static api_ctx_t g_api_ctx;

/* Initialization stage flags */
static int init_stage = 0;
#define INIT_STAGE_TPROXY     (1 << 0)
#define INIT_STAGE_ROUTING    (1 << 1)
#define INIT_STAGE_DNS        (1 << 2)
#define INIT_STAGE_GEOIP      (1 << 3)
#define INIT_STAGE_APP_FILTER (1 << 4)
#define INIT_STAGE_MAC_FILTER (1 << 5)
#define INIT_STAGE_PERF_MODE  (1 << 6)
#define INIT_STAGE_IPV6_MGR   (1 << 7)

/* Core features mask */
#define INIT_STAGE_CORE_MASK (INIT_STAGE_TPROXY | INIT_STAGE_ROUTING | INIT_STAGE_DNS)

/* Helper to set stage flag only on success */
static void set_stage_on_success(int *stage, int flag, const char *msg, int success) {
    if (success) {
        if (!(*stage & flag)) {
            LOG_INFO("%s", msg);
            *stage |= flag;
        }
    } else {
        LOG_ERROR("%s FAILED", msg);
    }
}

/* Check if core features are ready */
static int atp_core_ready(void) {
    return (init_stage & INIT_STAGE_CORE_MASK) == INIT_STAGE_CORE_MASK;
}

static void signal_handler(int sig) {
    if (sig == SIGTERM || sig == SIGINT) {
        LOG_INFO("Received signal %d, shutting down...", sig);
        g_running = 0;
    } else if (sig == SIGHUP) {
        LOG_INFO("Received SIGHUP, reloading configuration...");
        /* Reset API rate limit before reload to allow immediate sync */
        api_reset_rate_limit(&g_api_ctx);
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

int check_ip6tables_available(void) {
    if (access("/system/bin/ip6tables", X_OK) != 0) {
        return 0;
    }
    
    char output[256];
    int ret = exec_cmd("/system/bin/ip6tables -L INPUT -n 2>/dev/null | head -1", 
                       output, sizeof(output), 3);
    if (ret != 0) {
        return 0;
    }
    
    if (strstr(output, "can't initialize") || 
        strstr(output, "No chain") ||
        strstr(output, "Protocol not supported")) {
        return 0;
    }
    
    return 1;
}

int atp_init(void) {
    char dirs[3][PATH_MAX];
    snprintf(dirs[0], sizeof(dirs[0]), "%s/run", g_config.data_dir);
    snprintf(dirs[1], sizeof(dirs[1]), "%s/rules", g_config.data_dir);
    snprintf(dirs[2], sizeof(dirs[2]), "%s/bin", g_config.data_dir);
    
    for (int i = 0; i < 3; i++) {
        mkdir_recursive(dirs[i], 0755);
    }
    
    /* Check ip6tables availability and auto-disable IPv6 if not available */
    if (g_config.proxy_ipv6) {
        if (!check_ip6tables_available()) {
            LOG_WARN("ip6tables not available (binary missing or kernel module missing), IPv6 proxy will be disabled");
            g_config.proxy_ipv6 = 0;
        } else {
            LOG_INFO("ip6tables available, IPv6 proxy enabled");
        }
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
    pthread_mutex_destroy(&g_config.config_mutex);
}

void atp_show_status(void) {
    status_show(&g_config, &g_service_ctx, &g_api_ctx);
}

static int init_tasks(atp_config_t *cfg) {
    int core_success = 1;
    int tproxy_ok = 0, routing_ok = 0, dns_ok = 0;
    
    /* TPROXY rules */
    LOG_INFO("Setting up TPROXY rules");
    if (tproxy_setup_ipv4(cfg) == 0) {
        if (cfg->proxy_ipv6 && tproxy_setup_ipv6(cfg) != 0) {
            LOG_ERROR("Failed to setup IPv6 TPROXY rules");
        } else {
            tproxy_ok = 1;
            LOG_INFO("TPROXY rules setup completed");
        }
    } else {
        LOG_ERROR("Failed to setup IPv4 TPROXY rules");
    }
    
    /* Routing rules */
    LOG_INFO("Setting up routing rules");
    if (routing_setup_ipv4(cfg) == 0) {
        if (cfg->proxy_ipv6 && routing_setup_ipv6(cfg) != 0) {
            LOG_ERROR("Failed to setup IPv6 routing rules");
        } else {
            routing_ok = 1;
            LOG_INFO("Routing rules setup completed");
        }
    } else {
        LOG_ERROR("Failed to setup IPv4 routing rules");
    }
    
    /* Verify routing table exists before DNS hijack */
    char verify_cmd[256];
    snprintf(verify_cmd, sizeof(verify_cmd), 
             "ip route show table %d 2>/dev/null | grep -q '^local'", cfg->table_id);
    if (exec_cmd_simple(verify_cmd, 2) != 0) {
        LOG_WARN("Routing table %d not ready, waiting 100ms...", cfg->table_id);
        usleep(100000);
    }
    
    /* DNS hijack */
    LOG_INFO("Setting up DNS hijack");
    if (tproxy_dns_hijack_setup(cfg, 4, cfg->dns_hijack) == 0) {
        if (cfg->proxy_ipv6 && tproxy_dns_hijack_setup(cfg, 6, cfg->dns_hijack) != 0) {
            LOG_ERROR("Failed to setup IPv6 DNS hijack");
        } else {
            dns_ok = 1;
            LOG_INFO("DNS hijack setup completed");
        }
    } else {
        LOG_ERROR("Failed to setup IPv4 DNS hijack");
    }
    
    /* Update core stage flags only on success */
    if (tproxy_ok) init_stage |= INIT_STAGE_TPROXY;
    if (routing_ok) init_stage |= INIT_STAGE_ROUTING;
    if (dns_ok) init_stage |= INIT_STAGE_DNS;
    
    core_success = (tproxy_ok && routing_ok && dns_ok);
    
    /* GeoIP (async, non-blocking) */
    if (cfg->bypass_cn_ip) {
        LOG_INFO("Setting up GeoIP (async mode)");
        geoip_setup_ipset_async(cfg);
    } else {
        init_stage |= INIT_STAGE_GEOIP;
    }
    
    /* Optional features - these don't affect core readiness */
    LOG_INFO("Setting up performance mode");
    perf_mode_init(cfg);
    perf_mode_setup(cfg);
    init_stage |= INIT_STAGE_PERF_MODE;
    
    LOG_INFO("Setting up app filter");
    app_filter_init(cfg);
    app_filter_setup(cfg);
    init_stage |= INIT_STAGE_APP_FILTER;
    
    LOG_INFO("Setting up MAC filter");
    mac_filter_init(cfg);
    mac_filter_setup(cfg);
    init_stage |= INIT_STAGE_MAC_FILTER;
    
    LOG_INFO("Setting up IPv6 manager");
    ipv6_manager_init(cfg);
    init_stage |= INIT_STAGE_IPV6_MGR;
    
    if (core_success) {
        LOG_INFO("Core initialization completed successfully");
    } else {
        LOG_WARN("Core initialization incomplete, entering degraded mode");
    }
    
    return core_success ? 0 : -1;
}

int main(int argc, char *argv[]) {
    atp_options_t opts;
    memset(&opts, 0, sizeof(opts));
    
    if (parse_arguments(argc, argv, &opts) != 0) {
        return 1;
    }
    
    if (opts.command == CMD_HELP) {
        print_help(argv[0]);
        return 0;
    }
    
    if (opts.command == CMD_VERSION) {
        print_version();
        return 0;
    }
    
    /* Handle status command - no log initialization needed */
    if (opts.command == CMD_STATUS) {
        config_set_defaults(&g_config);
        if (opts.config_dir[0]) {
            strncpy(g_config.data_dir, opts.config_dir, sizeof(g_config.data_dir) - 1);
            g_config.data_dir[sizeof(g_config.data_dir) - 1] = '\0';
        }
        service_init(&g_service_ctx, &g_config);
        atp_show_status();
        return 0;
    }
    
    /* Root check for all other commands */
    if (atp_check_root() != 0) {
        return 1;
    }
    
    log_init();
    if (opts.verbose) log_set_level(LOG_LEVEL_DEBUG);
    if (opts.quiet) log_set_level(LOG_LEVEL_ERROR);
    
    LOG_INFO(ATP_NAME " v" ATP_VERSION " starting");
    
    config_set_defaults(&g_config);
    
    if (opts.config_dir[0]) {
        strncpy(g_config.data_dir, opts.config_dir, sizeof(g_config.data_dir) - 1);
        g_config.data_dir[sizeof(g_config.data_dir) - 1] = '\0';
    }
    
    g_config.dry_run = opts.dry_run;
    g_config.verbose = opts.verbose;
    g_config.foreground = opts.foreground;
    
    char conf_path[PATH_MAX];
    snprintf(conf_path, sizeof(conf_path), "%s/%s", 
             g_config.data_dir, ATP_CONF_FILE);
    
    if (file_exists(conf_path)) {
        config_load(conf_path, &g_config);
        LOG_INFO("Configuration loaded from %s", conf_path);
    } else {
        LOG_WARN("Config file not found: %s, using defaults", conf_path);
    }
    
    if (opts.command == CMD_UPDATE_GEOIP) {
        geoip_init(&g_config);
        geoip_force_update(&g_config);
        return 0;
    }
    
    if (opts.command == CMD_STOP) {
        service_init(&g_service_ctx, &g_config);
        service_stop(&g_service_ctx);
        tproxy_cleanup_all(&g_config);
        routing_cleanup_all(&g_config);
        app_filter_cleanup(&g_config);
        mac_filter_cleanup(&g_config);
        LOG_INFO("ATP stopped");
        return 0;
    }
    
    LOG_INFO("Using ENHANCE mode (Split TCP:NAT / UDP:Mangle)");
    
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
    
    /* Initialize all network rules synchronously */
    init_tasks(&g_config);
    
    /* Additional setup that must run after main tasks */
    tproxy_block_quic(&g_config, g_config.block_quic);
    tproxy_block_loopback(&g_config, 1);
    tproxy_xfrm_bypass(&g_config);
    tproxy_prevent_loop(&g_config);
    
    int heal_counter = 0;
    int geoip_ready_logged = 0;
    time_t last_api_sync = 0;
    
    while (g_running) {
        sleep(10);
        
        /* Self-heal: only run when core features are ready */
        if (atp_core_ready()) {
            heal_counter++;
            if (heal_counter >= 3) {
                heal_counter = 0;
                char vpn_iface[IFNAMSIZ];
                if (netlink_get_active_vpn(vpn_iface, sizeof(vpn_iface)) == 0) {
                    if (!netlink_check_rule_exists(g_config.table_id, g_config.mark_value, vpn_iface)) {
                        LOG_WARN("Rule drift detected, repairing...");
                        routing_add_vpn_policy(&g_config, vpn_iface);
                        routing_add_mss_clamp(&g_config, vpn_iface);
                    }
                }
            }
        } else {
            /* Log degraded mode periodically */
            static int degraded_log_counter = 0;
            if (++degraded_log_counter >= 60) {
                degraded_log_counter = 0;
                LOG_WARN("Degraded mode: core features not ready (stage=0x%x)", init_stage);
            }
        }
        
        /* Log GeoIP ready status once */
        if (!geoip_ready_logged && geoip_async_is_complete()) {
            LOG_INFO("GeoIP async initialization completed");
            init_stage |= INIT_STAGE_GEOIP;
            geoip_ready_logged = 1;
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
    
    service_stop(&g_service_ctx);
    tproxy_cleanup_all(&g_config);
    routing_cleanup_all(&g_config);
    app_filter_cleanup(&g_config);
    mac_filter_cleanup(&g_config);
    
    atp_cleanup();
    LOG_INFO(ATP_NAME " stopped");
    
    return 0;
}
