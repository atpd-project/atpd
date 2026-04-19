#include "atp.h"
#include "logger.h"
#include "config.h"
#include "tproxy.h"
#include "routing.h"
#include "netlink.h"
#include "netlink_monitor.h"
#include "fcm_monitor.h"
#include "service.h"
#include "geoip.h"
#include "api.h"
#include "cli.h"
#include "status.h"
#include "app_filter.h"
#include "mac_filter.h"
#include "ipv6_manager.h"
#include "perf_mode.h"
#include "inet_diag.h"
#include "version.h"
#include "ui.h"
#include "utils.h"
#include <libgen.h>
#include <sys/stat.h>
#include <sys/wait.h>

atp_config_t g_config;

static volatile sig_atomic_t g_running = 1;
static service_ctx_t g_service_ctx;
api_ctx_t g_api_ctx;

/* VPN-aware health check state */
static int g_vpn_connected = 0;
static int g_health_check_counter = 0;
#define HEALTH_CHECK_NORMAL_INTERVAL 1   /* Check every 10 seconds */
#define HEALTH_CHECK_VPN_INTERVAL 6      /* Check every 60 seconds when VPN is active */

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

static const char *state_colors[] = {
    [STATE_UNINITIALIZED] = "\033[1;90m",
    [STATE_INIT]          = "\033[1;36m",
    [STATE_CORE_START]    = "\033[1;36m",
    [STATE_CORE_WAIT]     = "\033[1;33m",
    [STATE_RULES_DEPLOY]  = "\033[1;34m",
    [STATE_READY]         = "\033[1;32m",
    [STATE_DEGRADED]      = "\033[1;33m",
    [STATE_RECOVER]       = "\033[1;31m",
    [STATE_STOPPING]      = "\033[1;31m"
};

/* Forward declarations */
static int atomic_mode_switch(atp_config_t *cfg, api_ctx_t *api, const char *new_mode);
static void on_fcm_connection(const char *remote_ip, uint16_t remote_port, void *userdata);

/* Print ASCII art banner with version info */
static void print_banner(void) {
    printf("\033[1;36m"
    "    ___  __________  ____ \n"
    "   /   |/_  __/ __ \\/ __ \\\n"
    "  / /| | / / / /_/ / / / /\n"
    " / ___ |/ / / ____/ /_/ / \n"
    "/_/  |_/_/ /_/    /_____/  v%s\033[0m\n", 
    atp_get_version());
    
    printf("--------------------------------------------\n");
    printf(" Version:   %s\n", atp_get_full_version());
    printf(" Build:     %s | %s\n", atp_get_build_date(), atp_get_build_time());
    printf(" Compiler:  %s\n", atp_get_compiler());
    printf(" Arch:      %s\n", atp_get_arch());
    
    if (atp_is_dirty()) {
        printf("\033[1;33m Warning:  Uncommitted changes present\033[0m\n");
    }
    if (!atp_is_clean()) {
        printf("\033[1;33m Note:     Working tree has modifications\033[0m\n");
    }
    
    printf(" Environment: Root (KernelSU)\n");
    printf(" Libs: musl-static | cJSON | libcurl\n");
    printf(" Features: FCM Monitor | Netlink Monitor | VPN-Aware Health\n");
    printf("--------------------------------------------\n\n");
}

/* Print startup health summary table using UI module */
static void print_startup_summary(void) {
    extern int g_current_uids_count;
    extern int *g_current_uids;
    char vpn_iface[IFNAMSIZ];
    char status_buf[64];
    char pid_buf[32];

    ui_table_sep();
    ui_table_header("ATP STARTUP SUMMARY");

    /* TPROXY Rules */
    if (init_stage & INIT_STAGE_TPROXY) {
        ui_status_ok("TPROXY Rules");
    } else {
        ui_status_fail("TPROXY Rules");
    }

    /* Routing Policy */
    if (init_stage & INIT_STAGE_ROUTING) {
        snprintf(status_buf, sizeof(status_buf), "Table %d fwmark rules", g_config.table_id);
        ui_table_subrow("├─", "Routing Policy", status_buf);
    } else {
        ui_status_fail("Routing Policy");
    }

    /* DNS Hijack */
    if (init_stage & INIT_STAGE_DNS) {
        snprintf(status_buf, sizeof(status_buf), "Port %d redirected", g_config.dns_port);
        ui_table_subrow("├─", "DNS Hijack", status_buf);
    } else {
        ui_status_fail("DNS Hijack");
    }

    /* GeoIP (CN Bypass) */
    if (init_stage & INIT_STAGE_GEOIP) {
        ui_status_ok("GeoIP (CN Bypass)");
    } else if (g_config.bypass_cn_ip) {
        ui_table_subrow_color("├─", "GeoIP (CN Bypass)", "ASYNC - Download in background", COLOR_YELLOW);
    } else {
        ui_status_off("GeoIP (CN Bypass)");
    }

    /* App Filter */
    if (init_stage & INIT_STAGE_APP_FILTER) {
        snprintf(status_buf, sizeof(status_buf), "%d UIDs in ipset", 
                 g_current_uids ? g_current_uids_count : 0);
        ui_table_subrow("├─", "App Filter", status_buf);
    } else {
        ui_status_off("App Filter");
    }

    /* MAC Filter */
    if (init_stage & INIT_STAGE_MAC_FILTER) {
        ui_status_ok("MAC Filter");
    } else {
        ui_status_off("MAC Filter");
    }

    /* Performance Mode */
    if (init_stage & INIT_STAGE_PERF_MODE) {
        ui_status_ok("Performance Mode");
    } else {
        ui_status_off("Performance Mode");
    }

    /* IPv6 Manager */
    if (init_stage & INIT_STAGE_IPV6_MGR) {
        ui_status_ok("IPv6 Manager");
    } else if (g_config.proxy_ipv6) {
        ui_table_subrow_color("├─", "IPv6 Manager", "PARTIAL - Check ip6tables", COLOR_YELLOW);
    } else {
        ui_status_off("IPv6 Manager");
    }

    ui_table_sep();

    /* VPN detection status */
    if (netlink_get_active_vpn(vpn_iface, sizeof(vpn_iface)) == 0 && vpn_iface[0]) {
        snprintf(status_buf, sizeof(status_buf), "Interface: %s", vpn_iface);
        ui_table_subrow_emoji_color(ui_emoji_vpn(1), "VPN Connected", status_buf, COLOR_GREEN);
        ui_table_subrow("└─", "XFRM Bypass", "OK - IPsec/ESP fast lane");
    } else {
        ui_table_subrow_emoji_color(ui_emoji_vpn(0), "VPN Connected", "No active VPN tunnel", COLOR_WHITE);
    }

    /* sing-box service status */
    if (service_check(&g_service_ctx)) {
        int pid = service_get_pid(&g_service_ctx);
        snprintf(pid_buf, sizeof(pid_buf), "PID: %d", pid);
        ui_table_subrow_emoji(ui_emoji_service(1), "sing-box Service", pid_buf);
    } else {
        ui_table_subrow_emoji_color(ui_emoji_service(0), "sing-box Service", "Process not running", COLOR_RED);
    }

    ui_table_end();
}

/* Print quick health check using UI module */
static void print_health_check(void) {
    char check_cmd[256];
    char status_buf[64];

    ui_table_sep();
    ui_table_header("ATP HEALTH CHECK");

    /* Netlink Monitor */
    if (netlink_monitor_is_running()) {
        ui_table_subrow_emoji_color("✓", "Netlink Monitor", "OK - Async ready", COLOR_GREEN);
    } else {
        ui_table_subrow_emoji_color("✗", "Netlink Monitor", "FAIL - Not running", COLOR_RED);
    }

    /* FCM Monitor */
    if (fcm_monitor_is_running()) {
        ui_table_subrow_emoji_color("✓", "FCM Monitor", "OK - Detecting Google VPN", COLOR_GREEN);
    } else {
        ui_table_subrow_emoji_color("✗", "FCM Monitor", "FAIL - Not running", COLOR_RED);
    }

    /* TPROXY Rules */
    snprintf(check_cmd, sizeof(check_cmd), 
             "iptables -t mangle -L ATP_PRE_0 -n 2>/dev/null | head -1 | grep -q ATP_PRE_0");
    if (exec_cmd_simple(check_cmd, 2) == 0) {
        snprintf(status_buf, sizeof(status_buf), "OK - Table %d injected", g_config.table_id);
        ui_table_subrow_emoji_color("✓", "TPROXY Rules", status_buf, COLOR_GREEN);
    } else {
        ui_table_subrow_emoji_color("✗", "TPROXY Rules", "FAIL - Not configured", COLOR_RED);
    }

    /* Routing Table */
    snprintf(check_cmd, sizeof(check_cmd), 
             "ip route show table %d 2>/dev/null | grep -q local", g_config.table_id);
    if (exec_cmd_simple(check_cmd, 2) == 0) {
        snprintf(status_buf, sizeof(status_buf), "OK - Table %d locked", g_config.table_id);
        ui_table_subrow_emoji_color("✓", "Routing Table", status_buf, COLOR_GREEN);
    } else {
        ui_table_subrow_emoji_color("✗", "Routing Table", "FAIL - Not configured", COLOR_RED);
    }

    /* INET_DIAG */
    if (inet_diag_available()) {
        ui_table_subrow_emoji_color("✓", "INET_DIAG", "OK - SELinux allowed", COLOR_GREEN);
    } else {
        ui_table_subrow_emoji_color("✗", "INET_DIAG", "FAIL - SELinux blocked", COLOR_RED);
    }

    /* sing-box API */
    if (api_check_health(&g_api_ctx)) {
        ui_table_subrow_emoji_color("✓", "sing-box API", "OK - Heartbeat OK", COLOR_GREEN);
    } else {
        ui_table_subrow_emoji_color("✗", "sing-box API", "FAIL - Not responding", COLOR_RED);
    }

    ui_table_end();
}
static void state_transition(atp_state_t new_state) {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_str[16];
    strftime(time_str, sizeof(time_str), "%H:%M:%S", tm_info);
    
    const char *color = state_colors[new_state];
    const char *reset = "\033[0m";
    
    if (g_state != STATE_UNINITIALIZED && g_state_enter_time > 0) {
        int elapsed = (int)(now - g_state_enter_time);
        if (elapsed > 0) {
            printf("[%s] %s[STATE]%s %s -> %s%s%s (took %ds)\n", 
                   time_str, color, reset,
                   state_names[g_state], color, state_names[new_state], reset, elapsed);
        } else {
            printf("[%s] %s[STATE]%s %s -> %s%s%s\n", 
                   time_str, color, reset,
                   state_names[g_state], color, state_names[new_state], reset);
        }
    } else {
        printf("[%s] %s[STATE]%s %s -> %s%s%s\n", 
               time_str, color, reset,
               state_names[g_state], color, state_names[new_state], reset);
    }
    
    g_state = new_state;
    g_state_enter_time = now;
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

/* Initialize netlink monitor for VPN detection */
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

/* Initialize FCM monitor for Google VPN auto-detection */
static void init_fcm_monitor(atp_config_t *cfg) {
    if (fcm_monitor_start(on_fcm_connection, cfg) != 0) {
        LOG_WARN("Failed to start FCM monitor, Google VPN auto-switch disabled");
    } else {
        LOG_INFO("FCM monitor active, Google VPN detection enabled (1s polling)");
    }
}

/* Atomic mode switch with config confirmation - only when mode changed */
static int atomic_mode_switch(atp_config_t *cfg, api_ctx_t *api, const char *new_mode) {
    char current_mode[64];
    static char last_mode[64] = "";
    
    /* Skip if mode hasn't changed */
    if (api_get_mode(api, current_mode, sizeof(current_mode)) == 0) {
        if (strcmp(current_mode, new_mode) == 0) {
            LOG_DEBUG("Mode unchanged (%s), skipping switch", current_mode);
            return 0;
        }
    }
    
    /* Check if this is the same as last requested mode */
    if (strcmp(last_mode, new_mode) == 0) {
        LOG_DEBUG("Mode %s already attempted, skipping duplicate request", new_mode);
        return 0;
    }
    
    strncpy(last_mode, new_mode, sizeof(last_mode) - 1);
    last_mode[sizeof(last_mode) - 1] = '\0';
    
    LOG_INFO("Switching mode: %s -> %s", current_mode, new_mode);
    
    if (api_set_mode(api, new_mode) != 0) {
        LOG_ERROR("Failed to set mode via API");
        last_mode[0] = '\0';
        return -1;
    }
    
    if (api_wait_for_config_loaded(api, new_mode, 3) != 0) {
        LOG_WARN("Config confirmation timeout, proceeding anyway");
    }
    
    /* Update routing based on new mode */
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

/* FCM callback - called when FCM connection detected */
static void on_fcm_connection(const char *remote_ip, uint16_t remote_port, void *userdata) {
    atp_config_t *cfg = (atp_config_t*)userdata;
    
    LOG_INFO("FCM connection to %s:%d detected, switching to Google VPN mode", 
             remote_ip, remote_port);
    
    /* Atomic mode switch */
    atomic_mode_switch(cfg, &g_api_ctx, "Google VPN");
}
static int init_tasks(atp_config_t *cfg) {
    int core_success = 1;
    int tproxy_ok = 0, routing_ok = 0, dns_ok = 0;
    
    /* Choose proxy mode based on configuration */
    switch (cfg->proxy_mode) {
        case MODE_ENHANCE:
            LOG_INFO("ENHANCE mode: TCP=REDIRECT (port %d), UDP=TPROXY (port %d)",
                     cfg->redirect_tcp_port, cfg->udp_port);
            if (tproxy_setup_enhance_ipv4(cfg) == 0) {
                if (cfg->proxy_ipv6 && tproxy_setup_enhance_ipv6(cfg) != 0) {
                    LOG_ERROR("Failed to setup IPv6 ENHANCE mode");
                } else {
                    tproxy_ok = 1;
                    LOG_INFO("ENHANCE mode setup completed");
                }
            } else {
                LOG_ERROR("Failed to setup IPv4 ENHANCE mode");
            }
            break;
            
        case MODE_TPROXY:
            LOG_INFO("TPROXY mode: TCP/UDP both use TPROXY");
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
            break;
            
        case MODE_REDIRECT:
            LOG_INFO("REDIRECT mode: TCP only (UDP not supported)");
            if (tproxy_setup_redirect_ipv4(cfg) == 0) {
                if (cfg->proxy_ipv6 && tproxy_setup_redirect_ipv6(cfg) != 0) {
                    LOG_ERROR("Failed to setup IPv6 REDIRECT rules");
                } else {
                    tproxy_ok = 1;
                    LOG_INFO("REDIRECT rules setup completed");
                }
            } else {
                LOG_ERROR("Failed to setup IPv4 REDIRECT rules");
            }
            break;
            
        case MODE_AUTO:
        default:
            if (tproxy_support_check(cfg)) {
                LOG_INFO("Auto mode: TPROXY supported, using TPROXY");
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
            } else {
                LOG_INFO("Auto mode: TPROXY not supported, falling back to REDIRECT");
                if (tproxy_setup_redirect_ipv4(cfg) == 0) {
                    if (cfg->proxy_ipv6 && tproxy_setup_redirect_ipv6(cfg) != 0) {
                        LOG_ERROR("Failed to setup IPv6 REDIRECT rules");
                    } else {
                        tproxy_ok = 1;
                        LOG_INFO("REDIRECT rules setup completed");
                    }
                } else {
                    LOG_ERROR("Failed to setup IPv4 REDIRECT rules");
                }
            }
            break;
    }
    
    /* Routing rules (same for all modes) */
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

int main(int argc, char *argv[]) {
    atp_options_t opts;
    memset(&opts, 0, sizeof(opts));
    
    /* Initialize UI module for consistent output */
    ui_init();
    
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
    
    /* Print banner on startup */
    print_banner();
    
    log_init();
    if (opts.verbose) log_set_level(LOG_LEVEL_DEBUG);
    if (opts.quiet) log_set_level(LOG_LEVEL_ERROR);
    
    LOG_INFO(ATP_NAME " v%s starting", atp_get_version());
    
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
        fcm_monitor_stop();
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
    
    int skip_sleep = 0;
    
    while (g_running) {
        switch (g_state) {
            case STATE_UNINITIALIZED:
                state_transition(STATE_INIT);
                skip_sleep = 1;
                break;
                
            case STATE_INIT:
                atp_init();
                init_netlink_monitor(&g_config);
                init_fcm_monitor(&g_config);
                service_init(&g_service_ctx, &g_config);
                api_init(&g_api_ctx, &g_config);
                state_transition(STATE_CORE_START);
                skip_sleep = 1;
                break;
                
            case STATE_CORE_START:
                LOG_INFO("Starting core service...");
                if (service_start(&g_service_ctx) == 0) {
                    state_transition(STATE_CORE_WAIT);
                } else {
                    LOG_ERROR("Failed to start core service");
                    skip_sleep = 1;
                }
                skip_sleep = 1;
                break;
                
            case STATE_CORE_WAIT:
                if (api_check_health(&g_api_ctx)) {
                    LOG_INFO("API health check passed");
                    state_transition(STATE_RULES_DEPLOY);
                    skip_sleep = 1;
                } else if (time(NULL) - g_state_enter_time > 30) {
                    LOG_ERROR("Core start timeout, retrying");
                    state_transition(STATE_CORE_START);
                    skip_sleep = 1;
                }
                break;
                
            case STATE_RULES_DEPLOY:
                if (init_tasks(&g_config) == 0) {
                    tproxy_block_quic(&g_config, g_config.block_quic);
                    tproxy_block_loopback(&g_config, 1);
                    tproxy_xfrm_bypass(&g_config);
                    tproxy_prevent_loop(&g_config);
                    
                    /* Print startup summary when entering READY state */
                    print_startup_summary();
                    print_health_check();
                    
                    state_transition(STATE_READY);
                } else {
                    LOG_ERROR("Rules deployment failed, entering degraded mode");
                    state_transition(STATE_DEGRADED);
                }
                skip_sleep = 1;
                break;
                
            case STATE_READY:
            {
                /* Check VPN status */
                char vpn_iface[IFNAMSIZ];
                int vpn_detected = (netlink_get_active_vpn(vpn_iface, sizeof(vpn_iface)) == 0 && vpn_iface[0]);
                
                /* VPN state change detection */
                if (vpn_detected != g_vpn_connected) {
                    if (vpn_detected) {
                        LOG_INFO("VPN connected, reducing health check frequency to 60s");
                    } else {
                        LOG_INFO("VPN disconnected, restoring health check frequency to 10s");
                        /* Ensure sing-box is available when VPN disconnects */
                        if (!service_check(&g_service_ctx)) {
                            LOG_WARN("sing-box not running after VPN disconnect, starting...");
                            service_start(&g_service_ctx);
                            service_wait_ready(&g_service_ctx, 10);
                        }
                    }
                    g_vpn_connected = vpn_detected;
                    g_health_check_counter = 0;
                }
                
                /* Adaptive health check based on VPN status */
                g_health_check_counter++;
                int check_interval = g_vpn_connected ? HEALTH_CHECK_VPN_INTERVAL : HEALTH_CHECK_NORMAL_INTERVAL;
                
                if (g_health_check_counter >= check_interval) {
                    g_health_check_counter = 0;
                    
                    if (!api_check_health(&g_api_ctx)) {
                        if (!g_vpn_connected) {
                            LOG_ERROR("Heartbeat lost and VPN not connected, entering recovery");
                            state_transition(STATE_RECOVER);
                            skip_sleep = 1;
                            break;
                        } else {
                            LOG_DEBUG("Heartbeat lost but VPN connected, skipping recovery");
                        }
                    }
                }
                
                /* Service health - only critical when VPN is not connected */
                if (!g_vpn_connected) {
                    if (!service_check(&g_service_ctx)) {
                        LOG_ERROR("sing-box is offline and VPN not connected!");
                        if (!service_cooldown_active(&g_service_ctx)) {
                            service_restart(&g_service_ctx);
                        }
                    }
                } else {
                    /* VPN connected: just log once if service is down */
                    static int service_down_logged = 0;
                    if (!service_check(&g_service_ctx)) {
                        if (!service_down_logged) {
                            LOG_WARN("sing-box offline but VPN connected, traffic unaffected");
                            service_down_logged = 1;
                        }
                    } else {
                        service_down_logged = 0;
                    }
                }
                
                /* Rule drift detection (only needed when VPN not connected) */
                if (!g_vpn_connected) {
                    static int heal_counter = 0;
                    heal_counter++;
                    if (heal_counter >= 3) {
                        heal_counter = 0;
                        if (netlink_get_active_vpn(vpn_iface, sizeof(vpn_iface)) == 0) {
                            if (!netlink_check_rule_exists(g_config.table_id, g_config.mark_value, vpn_iface)) {
                                LOG_WARN("Rule drift detected, repairing...");
                                routing_add_vpn_policy(&g_config, vpn_iface);
                                routing_add_mss_clamp(&g_config, vpn_iface);
                            }
                        }
                    }
                }
                
                /* GeoIP status check */
                {
                    static int geoip_ready_logged = 0;
                    if (!geoip_ready_logged && geoip_async_is_complete()) {
                        LOG_INFO("GeoIP async initialization completed");
                        init_stage |= INIT_STAGE_GEOIP;
                        geoip_ready_logged = 1;
                    }
                }
                
                /* API mode sync */
                {
                    static time_t last_api_sync = 0;
                    if (time(NULL) - last_api_sync > 300) {
                        atomic_mode_switch(&g_config, &g_api_ctx, g_config.user_clash_mode);
                        last_api_sync = time(NULL);
                    }
                }
                break;
            }
                
            case STATE_DEGRADED:
                LOG_WARN("Degraded mode: core features not ready (stage=0x%x)", init_stage);
                if (api_check_health(&g_api_ctx)) {
                    LOG_INFO("API recovered, attempting rule redeploy");
                    state_transition(STATE_RULES_DEPLOY);
                    skip_sleep = 1;
                } else if (!service_check(&g_service_ctx)) {
                    LOG_ERROR("Service offline in degraded mode, restarting");
                    state_transition(STATE_RECOVER);
                    skip_sleep = 1;
                }
                break;
                
            case STATE_RECOVER:
                LOG_INFO("Entering recovery mode, cleaning up...");
                routing_cleanup_all(&g_config);
                tproxy_cleanup_all(&g_config);
                app_filter_cleanup(&g_config);
                mac_filter_cleanup(&g_config);
                ipv6_manager_set_mode(&g_config, IPV6_MODE_DEFAULT);
                
                /* Only stop service if VPN is not connected */
                if (!g_vpn_connected) {
                    service_stop_graceful(&g_service_ctx, 3);
                } else {
                    LOG_INFO("VPN connected, skipping service stop");
                }
                
                init_stage = 0;
                state_transition(STATE_CORE_START);
                skip_sleep = 1;
                break;
                
            case STATE_STOPPING:
                goto exit_loop;
        }
        
        if (!skip_sleep) {
            sleep(10);
        } else {
            skip_sleep = 0;
        }
    }
    
exit_loop:
    service_stop_graceful(&g_service_ctx, 3);
    netlink_monitor_stop();
    fcm_monitor_stop();
    tproxy_cleanup_all(&g_config);
    routing_cleanup_all(&g_config);
    app_filter_cleanup(&g_config);
    mac_filter_cleanup(&g_config);
    
    atp_cleanup();
    LOG_INFO(ATP_NAME " stopped");
    
    return 0;
}
