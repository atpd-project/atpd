#include "atp.h"
#include "logger.h"
#include "config.h"
#include "tproxy.h"
#include "routing.h"
#include "netlink.h"
#include "netlink_monitor.h"
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
api_ctx_t g_api_ctx;

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

/* State machine definition */
typedef enum {
    STATE_UNINITIALIZED = 0,
    STATE_INIT,
    STATE_CORE_START,
    STATE_CORE_WAIT,
    STATE_RULES_DEPLOY,
    STATE_READY,
    STATE_DEGRADED,
    STATE_RECOVER,
    STATE_STOPPING
} atp_state_t;

static atp_state_t g_state = STATE_UNINITIALIZED;
static time_t g_state_enter_time = 0;

static const char *state_names[] = {
    "UNINITIALIZED", "INIT", "CORE_START", "CORE_WAIT",
    "RULES_DEPLOY", "READY", "DEGRADED", "RECOVER", "STOPPING"
};

static void state_transition(atp_state_t new_state) {
    LOG_INFO("State transition: %s -> %s", 
             state_names[g_state], state_names[new_state]);
    g_state = new_state;
    g_state_enter_time = time(NULL);
}

/* Check if core features are ready */
static int atp_core_ready(void) {
    return (init_stage & INIT_STAGE_CORE_MASK) == INIT_STAGE_CORE_MASK;
}

static void signal_handler(int sig) {
    if (sig == SIGTERM || sig == SIGINT) {
        LOG_INFO("Received signal %d, shutting down...", sig);
        g_running = 0;
        state_transition(STATE_STOPPING);
    } else if (sig == SIGHUP) {
        LOG_INFO("Received SIGHUP, reloading configuration...");
        api_reset_rate_limit(&g_api_ctx);
        config_reload(&g_config);
    }
}

int atp_signal_setup(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    
    if (sigaction(SIGTERM, &sa, NULL) < 0) return -1;
    if (sigaction(SIGINT, &sa, NULL) < 0) return -1;
    if (sigaction(SIGHUP, &sa, NULL) < 0) return -1;
    
    /* Auto-reap child processes without creating zombies */
    /* Linux 2.6+ will automatically reap children when SIGCHLD is ignored */
    signal(SIGCHLD, SIG_IGN);
    
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
    
    if (g_config.proxy_ipv6) {
        if (!check_ip6tables_available()) {
            LOG_WARN("ip6tables not available, IPv6 proxy will be disabled");
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

static void init_netlink_monitor(atp_config_t *cfg) {
    nl_monitor_config_t monitor_config;
    
    memset(&monitor_config, 0, sizeof(monitor_config));
    monitor_config.callback = netlink_default_callback;
    monitor_config.userdata = cfg;
    monitor_config.monitor_links = 1;
    monitor_config.monitor_addrs = 1;
    monitor_config.monitor_routes = 0;
    monitor_config.monitor_vpn_only = 1;
    
    if (netlink_monitor_start(&monitor_config) != 0) {
        LOG_WARN("Failed to start netlink monitor, falling back to polling mode");
    } else {
        LOG_INFO("Netlink monitor active, VPN handover latency reduced to microseconds");
    }
}

static int init_tasks(atp_config_t *cfg) {
    int core_success = 1;
    int tproxy_ok = 0, routing_ok = 0, dns_ok = 0;
    
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
    
    char verify_cmd[256];
    snprintf(verify_cmd, sizeof(verify_cmd), 
             "ip route show table %d 2>/dev/null | grep -q '^local'", cfg->table_id);
    if (exec_cmd_simple(verify_cmd, 2) != 0) {
        LOG_WARN("Routing table %d not ready, waiting 100ms...", cfg->table_id);
        usleep(100000);
    }
    
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
    
    if (tproxy_ok) init_stage |= INIT_STAGE_TPROXY;
    if (routing_ok) init_stage |= INIT_STAGE_ROUTING;
    if (dns_ok) init_stage |= INIT_STAGE_DNS;
    
    core_success = (tproxy_ok && routing_ok && dns_ok);
    
    if (cfg->bypass_cn_ip) {
        LOG_INFO("Setting up GeoIP (async mode)");
        geoip_setup_ipset_async(cfg);
    } else {
        init_stage |= INIT_STAGE_GEOIP;
    }
    
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

/* Atomic mode switch with config confirmation */
static int atomic_mode_switch(atp_config_t *cfg, api_ctx_t *api, const char *new_mode) {
    if (api_set_mode(api, new_mode) != 0) {
        LOG_ERROR("Failed to set mode via API");
        return -1;
    }
    
    if (api_wait_for_config_loaded(api, new_mode, 3) != 0) {
        LOG_WARN("Config confirmation timeout, proceeding anyway");
    }
    
    if (strcmp(new_mode, "Global") == 0 || strcmp(new_mode, "Google VPN") == 0) {
        char vpn_iface[IFNAMSIZ];
        if (netlink_get_active_vpn(vpn_iface, sizeof(vpn_iface)) == 0) {
            routing_add_vpn_policy(cfg, vpn_iface);
            routing_add_mss_clamp(cfg, vpn_iface);
        }
    } else {
        routing_cleanup_all(cfg);
        routing_setup_ipv4(cfg);
        if (cfg->proxy_ipv6) routing_setup_ipv6(cfg);
    }
    
    return 0;
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
        service_stop_graceful(&g_service_ctx, 3);
        netlink_monitor_stop();
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
    
    state_transition(STATE_INIT);
    
    while (g_running) {
        switch (g_state) {
            case STATE_UNINITIALIZED:
                state_transition(STATE_INIT);
                break;
                
            case STATE_INIT:
                atp_init();
                init_netlink_monitor(&g_config);
                service_init(&g_service_ctx, &g_config);
                api_init(&g_api_ctx, &g_config);
                state_transition(STATE_CORE_START);
                break;
                
            case STATE_CORE_START:
                LOG_INFO("Starting core service...");
                if (service_start(&g_service_ctx) == 0) {
                    state_transition(STATE_CORE_WAIT);
                } else {
                    LOG_ERROR("Failed to start core service");
                    sleep(5);
                }
                break;
                
            case STATE_CORE_WAIT:
                if (api_check_health(&g_api_ctx)) {
                    LOG_INFO("API health check passed");
                    state_transition(STATE_RULES_DEPLOY);
                } else if (time(NULL) - g_state_enter_time > 30) {
                    LOG_ERROR("Core start timeout, retrying");
                    state_transition(STATE_CORE_START);
                }
                break;
                
            case STATE_RULES_DEPLOY:
                if (init_tasks(&g_config) == 0) {
                    tproxy_block_quic(&g_config, g_config.block_quic);
                    tproxy_block_loopback(&g_config, 1);
                    tproxy_xfrm_bypass(&g_config);
                    tproxy_prevent_loop(&g_config);
                    state_transition(STATE_READY);
                } else {
                    LOG_ERROR("Rules deployment failed, entering degraded mode");
                    state_transition(STATE_DEGRADED);
                }
                break;
                
            case STATE_READY:
                if (!api_check_health(&g_api_ctx)) {
                    LOG_ERROR("Heartbeat lost, entering recovery");
                    state_transition(STATE_RECOVER);
                    break;
                }
                
                {
                    static int heal_counter = 0;
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
                }
                
                {
                    static int geoip_ready_logged = 0;
                    if (!geoip_ready_logged && geoip_async_is_complete()) {
                        LOG_INFO("GeoIP async initialization completed");
                        init_stage |= INIT_STAGE_GEOIP;
                        geoip_ready_logged = 1;
                    }
                }
                
                {
                    static time_t last_api_sync = 0;
                    if (time(NULL) - last_api_sync > 300) {
                        atomic_mode_switch(&g_config, &g_api_ctx, g_config.user_clash_mode);
                        last_api_sync = time(NULL);
                    }
                }
                break;
                
            case STATE_DEGRADED:
                LOG_WARN("Degraded mode: core features not ready (stage=0x%x)", init_stage);
                if (api_check_health(&g_api_ctx)) {
                    LOG_INFO("API recovered, attempting rule redeploy");
                    state_transition(STATE_RULES_DEPLOY);
                } else if (!service_check(&g_service_ctx)) {
                    LOG_ERROR("Service offline in degraded mode, restarting");
                    state_transition(STATE_RECOVER);
                }
                break;
                
            case STATE_RECOVER:
                LOG_INFO("Entering recovery mode, cleaning up...");
                routing_cleanup_all(&g_config);
                tproxy_cleanup_all(&g_config);
                service_stop_graceful(&g_service_ctx, 3);
                state_transition(STATE_CORE_START);
                break;
                
            case STATE_STOPPING:
                goto exit_loop;
        }
        
        sleep(10);
    }
    
exit_loop:
    service_stop_graceful(&g_service_ctx, 3);
    netlink_monitor_stop();
    tproxy_cleanup_all(&g_config);
    routing_cleanup_all(&g_config);
    app_filter_cleanup(&g_config);
    mac_filter_cleanup(&g_config);
    
    atp_cleanup();
    LOG_INFO(ATP_NAME " stopped");
    
    return 0;
}
