#include "status.h"
#include "logger.h"
#include "utils.h"
#include "netlink.h"
#include "netlink_monitor.h"
#include "service.h"
#include "api.h"
#include "fcm_monitor.h"
#include "perf_mode.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>

#define TRAFFIC_STATE_FILE "/data/adb/atp/run/traffic.state"
#define THERMAL_ZONE_BASE "/sys/class/thermal"
#define THERMAL_TEMP_WARN 75000
#define THERMAL_TEMP_CRITICAL 85000

/* External reference for service context */
extern service_ctx_t g_service_ctx;
extern api_ctx_t g_api_ctx;

/* Use logger.h colors - no redefinition needed */
/* COLOR_RESET, COLOR_RED, COLOR_GREEN, COLOR_YELLOW, COLOR_CYAN are from logger.h */

static const char* proxy_mode_to_string(atp_config_t *cfg) {
    switch (cfg->proxy_mode) {
        case 0: return cfg->use_tproxy ? "TPROXY (auto)" : "REDIRECT (auto)";
        case 1: return "TPROXY (TCP+UDP)";
        case 2: return "REDIRECT (TCP only)";
        case 3: return "ENHANCE (TCP=REDIRECT, UDP=TPROXY)";
        default: return "UNKNOWN";
    }
}

static void print_separator(void) {
    printf(COLOR_CYAN "┌─────────────────────────────────────────────────────────────────┐\n" COLOR_RESET);
}

static void print_table_header(const char *title) {
    printf(COLOR_CYAN "│ %-63s │\n" COLOR_RESET, title);
    printf(COLOR_CYAN "├─────────────────────────────────────────────────────────────────┤\n" COLOR_RESET);
}

static void print_table_row_with_color(const char *label, const char *value, const char *color) {
    printf("│ %-15s │ %s%-45s" COLOR_RESET " │\n", label, color, value);
}

static void print_table_subrow(const char *prefix, const char *label, const char *value) {
    printf("│  %s%-13s │ %-45s │\n", prefix, label, value);
}

static void print_table_subrow_with_color(const char *prefix, const char *label, const char *value, const char *color) {
    printf("│  %s%-13s │ %s%-45s" COLOR_RESET " │\n", prefix, label, color, value);
}

static void print_table_subrow_int(const char *prefix, const char *label, int value) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", value);
    print_table_subrow(prefix, label, buf);
}

static void print_table_end(void) {
    printf(COLOR_CYAN "└─────────────────────────────────────────────────────────────────┘\n" COLOR_RESET);
}

/* Format uptime in human readable format */
static void format_uptime_human(int seconds, char *buf, size_t size) {
    int days = seconds / 86400;
    int hours = (seconds % 86400) / 3600;
    int mins = (seconds % 3600) / 60;
    int secs = seconds % 60;

    if (days > 0) {
        snprintf(buf, size, "%dd %02d:%02d:%02d", days, hours, mins, secs);
    } else if (hours > 0) {
        snprintf(buf, size, "%dh %02dm %02ds", hours, mins, secs);
    } else if (mins > 0) {
        snprintf(buf, size, "%dm %02ds", mins, secs);
    } else {
        snprintf(buf, size, "%ds", secs);
    }
}

/* Format bytes to human readable string */
static void format_bytes(char *buf, size_t size, unsigned long long bytes) {
    if (bytes >= 1024 * 1024 * 1024) {
        snprintf(buf, size, "%.2f GB", (double)bytes / (1024 * 1024 * 1024));
    } else if (bytes >= 1024 * 1024) {
        snprintf(buf, size, "%.2f MB", (double)bytes / (1024 * 1024));
    } else if (bytes >= 1024) {
        snprintf(buf, size, "%.2f KB", (double)bytes / 1024);
    } else {
        snprintf(buf, size, "%llu B", bytes);
    }
}

/* Format speed to human readable string */
static void format_speed(char *buf, size_t size, unsigned long long bytes_per_sec) {
    unsigned long long bits_per_sec = bytes_per_sec * 8;

    if (bits_per_sec >= 1024 * 1024 * 1024) {
        snprintf(buf, size, "%.2f Gbps", (double)bits_per_sec / (1024 * 1024 * 1024));
    } else if (bits_per_sec >= 1024 * 1024) {
        snprintf(buf, size, "%.2f Mbps", (double)bits_per_sec / (1024 * 1024));
    } else if (bits_per_sec >= 1024) {
        snprintf(buf, size, "%.2f Kbps", (double)bits_per_sec / 1024);
    } else {
        snprintf(buf, size, "%llu bps", bits_per_sec);
    }
}

/* Get CPU temperature in Celsius */
static int get_cpu_temperature(void) {
    DIR *dir;
    struct dirent *entry;
    char path[PATH_MAX];
    char temp_str[16];
    int temp = 0;

    dir = opendir(THERMAL_ZONE_BASE);
    if (!dir) return -1;

    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "thermal_zone", 12) == 0) {
            snprintf(path, sizeof(path), "%s/%s/temp", THERMAL_ZONE_BASE, entry->d_name);
            FILE *fp = fopen(path, "r");
            if (fp) {
                if (fgets(temp_str, sizeof(temp_str), fp)) {
                    temp = atoi(temp_str) / 1000;
                    fclose(fp);
                    closedir(dir);
                    return temp;
                }
                fclose(fp);
            }
        }
    }
    closedir(dir);
    return -1;
}

/* Show PROXY CORE module */
static void status_show_proxy_core(void) {
    int pid = service_get_pid(&g_service_ctx);
    char uptime_str[64];
    char mem_str[32];
    char cpu_str[16];
    char threads_str[16];
    char fds_str[16];
    char version_str[64];

    print_separator();
    print_table_header("PROXY CORE");

    if (pid <= 0) {
        print_table_row_with_color("STATUS", "sing-box", COLOR_RED);
        print_table_end();
        return;
    }

    long mem_kb = get_process_memory_kb(pid);
    double cpu = get_process_cpu_percent(pid);
    int threads = get_process_threads(pid);
    int fd_count = get_process_fd_count(pid);
    int uptime_sec = get_process_uptime_sec(pid);

    format_uptime_human(uptime_sec, uptime_str, sizeof(uptime_str));
    format_bytes(mem_str, sizeof(mem_str), mem_kb * 1024);
    snprintf(cpu_str, sizeof(cpu_str), "%.1f%%", cpu);
    snprintf(threads_str, sizeof(threads_str), "%d", threads);
    snprintf(fds_str, sizeof(fds_str), "%d", fd_count);
    get_binary_version(PROXY_BIN_PATH, version_str, sizeof(version_str));

    print_table_row_with_color("RUNNING", "sing-box", COLOR_GREEN);
    print_table_subrow_int("├─", "PID", pid);
    print_table_subrow("├─", "Uptime", uptime_str);
    print_table_subrow("├─", "Memory", mem_str);
    print_table_subrow("├─", "CPU", cpu_str);
    print_table_subrow("├─", "Threads", threads_str);
    print_table_subrow("├─", "FDs", fds_str);
    print_table_subrow("└─", "Version", version_str);

    print_table_end();
}

/* Show CLASH MODE module */
static void status_show_clash_mode(atp_config_t *cfg) {
    char current_mode[64] = {0};
    int api_ok = 0;

    print_separator();
    print_table_header("CLASH MODE");

    if (service_get_pid(&g_service_ctx) <= 0) {
        print_table_row_with_color("MODE", "N/A (service stopped)", COLOR_YELLOW);
        print_table_end();
        return;
    }

    if (api_get_mode(&g_api_ctx, current_mode, sizeof(current_mode)) == 0) {
        api_ok = 1;
    }

    if (api_ok) {
        const char *color = COLOR_GREEN;
        if (strcmp(current_mode, "Rule") == 0) color = COLOR_CYAN;
        else if (strcmp(current_mode, "Global") == 0) color = COLOR_YELLOW;
        else if (strcmp(current_mode, "Google VPN") == 0) color = COLOR_GREEN;
        print_table_row_with_color("MODE", current_mode, color);
    } else {
        print_table_row_with_color("MODE", cfg->user_clash_mode, COLOR_YELLOW);
        printf("│                 │ %s[cached, API unavailable]%-24s" COLOR_RESET " │\n", COLOR_YELLOW, "");
    }

    print_table_end();
}

/* Show MONITORS module */
static void status_show_monitors(void) {
    time_t last_fcm = fcm_monitor_get_last_detection();
    time_t now = time(NULL);
    char fcm_status[64];

    print_separator();
    print_table_header("MONITORS");

    /* Netlink Monitor */
    if (netlink_monitor_is_running()) {
        print_table_subrow_with_color("├─", "Netlink Monitor", "ACTIVE", COLOR_GREEN);
    } else {
        print_table_subrow_with_color("├─", "Netlink Monitor", "INACTIVE", COLOR_RED);
    }

    /* FCM Monitor */
    if (fcm_monitor_is_running()) {
        if (last_fcm > 0) {
            int elapsed = (int)(now - last_fcm);
            if (elapsed < 60) {
                snprintf(fcm_status, sizeof(fcm_status), "ACTIVE (last trigger: %ds ago)", elapsed);
            } else if (elapsed < 3600) {
                snprintf(fcm_status, sizeof(fcm_status), "ACTIVE (last trigger: %dm %ds ago)", elapsed / 60, elapsed % 60);
            } else {
                snprintf(fcm_status, sizeof(fcm_status), "ACTIVE (last trigger: %dh ago)", elapsed / 3600);
            }
            print_table_subrow_with_color("└─", "FCM Monitor", fcm_status, COLOR_GREEN);
        } else {
            print_table_subrow_with_color("└─", "FCM Monitor", "ACTIVE (waiting for FCM)", COLOR_CYAN);
        }
    } else {
        print_table_subrow_with_color("└─", "FCM Monitor", "INACTIVE", COLOR_RED);
    }

    print_table_end();
}

/* Traffic monitoring structures */
typedef struct {
    unsigned long long rx_bytes;
    unsigned long long tx_bytes;
    time_t timestamp;
    char iface[IFNAMSIZ];
} iface_stats_t;

static int get_iface_traffic(const char *iface, unsigned long long *rx_bytes, unsigned long long *tx_bytes) {
    FILE *fp = fopen("/proc/net/dev", "r");
    if (!fp) return -1;

    char line[512];
    int found = 0;

    fgets(line, sizeof(line), fp);
    fgets(line, sizeof(line), fp);

    while (fgets(line, sizeof(line), fp)) {
        char name[64];
        unsigned long long rx_bytes_val, tx_bytes_val;

        if (sscanf(line, "%63[^:]: %llu %*u %*u %*u %*u %*u %*u %*u %llu",
                   name, &rx_bytes_val, &tx_bytes_val) >= 3) {
            char *p = name;
            while (*p == ' ') p++;

            if (strcmp(p, iface) == 0) {
                *rx_bytes = rx_bytes_val;
                *tx_bytes = tx_bytes_val;
                found = 1;
                break;
            }
        }
    }

    fclose(fp);
    return found ? 0 : -1;
}

static int load_traffic_state(iface_stats_t *stats) {
    FILE *fp = fopen(TRAFFIC_STATE_FILE, "r");
    if (!fp) return -1;

    int ret = fscanf(fp, "%s %llu %llu %ld",
                     stats->iface,
                     &stats->rx_bytes,
                     &stats->tx_bytes,
                     (long*)&stats->timestamp);
    fclose(fp);

    return (ret == 4 && stats->iface[0] != '\0') ? 0 : -1;
}

static int save_traffic_state(const iface_stats_t *stats) {
    char dir[PATH_MAX];
    strncpy(dir, TRAFFIC_STATE_FILE, sizeof(dir) - 1);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        mkdir_recursive(dir, 0755);
    }

    FILE *fp = fopen(TRAFFIC_STATE_FILE, "w");
    if (!fp) return -1;

    fprintf(fp, "%s %llu %llu %ld\n",
            stats->iface,
            stats->rx_bytes,
            stats->tx_bytes,
            (long)stats->timestamp);
    fclose(fp);

    return 0;
}

/* Show VPN STATUS module */
static void status_show_vpn(void) {
    char vpn_iface[IFNAMSIZ] = {0};
    unsigned long long rx_bytes = 0, tx_bytes = 0;
    char rx_str[32], tx_str[32];
    char rx_speed_str[32], tx_speed_str[32];
    iface_stats_t current_stats;
    iface_stats_t prev_stats;
    int has_vpn = 0;

    print_separator();
    print_table_header("VPN STATUS");

    if (netlink_get_active_vpn(vpn_iface, sizeof(vpn_iface)) == 0 && vpn_iface[0]) {
        has_vpn = 1;
    }

    if (!has_vpn) {
        print_table_row_with_color("STATE", "DISCONNECTED", COLOR_RED);
        print_table_end();
        return;
    }

    print_table_subrow("├─", "Interface", vpn_iface);

    /* State */
    print_table_subrow_with_color("├─", "State", "CONNECTED", COLOR_GREEN);

    /* Traffic stats */
    if (get_iface_traffic(vpn_iface, &rx_bytes, &tx_bytes) == 0) {
        format_bytes(rx_str, sizeof(rx_str), rx_bytes);
        format_bytes(tx_str, sizeof(tx_str), tx_bytes);

        print_table_subrow("├─", "Total RX", rx_str);
        print_table_subrow("├─", "Total TX", tx_str);

        memset(&current_stats, 0, sizeof(current_stats));
        strncpy(current_stats.iface, vpn_iface, IFNAMSIZ - 1);
        current_stats.rx_bytes = rx_bytes;
        current_stats.tx_bytes = tx_bytes;
        current_stats.timestamp = time(NULL);

        if (load_traffic_state(&prev_stats) == 0 &&
            strcmp(prev_stats.iface, vpn_iface) == 0 &&
            prev_stats.timestamp > 0) {

            double elapsed = difftime(current_stats.timestamp, prev_stats.timestamp);

            if (elapsed >= 1.0 && elapsed <= 3600.0) {
                unsigned long long rx_diff = (current_stats.rx_bytes > prev_stats.rx_bytes) ?
                                             (current_stats.rx_bytes - prev_stats.rx_bytes) : 0;
                unsigned long long tx_diff = (current_stats.tx_bytes > prev_stats.tx_bytes) ?
                                             (current_stats.tx_bytes - prev_stats.tx_bytes) : 0;

                unsigned long long rx_speed = (unsigned long long)((double)rx_diff / elapsed);
                unsigned long long tx_speed = (unsigned long long)((double)tx_diff / elapsed);

                format_speed(rx_speed_str, sizeof(rx_speed_str), rx_speed);
                format_speed(tx_speed_str, sizeof(tx_speed_str), tx_speed);

                char speed_info[64];
                snprintf(speed_info, sizeof(speed_info), "%s (over %.0fs)", rx_speed_str, elapsed);
                print_table_subrow("├─", "Avg RX Speed", speed_info);
                snprintf(speed_info, sizeof(speed_info), "%s (over %.0fs)", tx_speed_str, elapsed);
                print_table_subrow("└─", "Avg TX Speed", speed_info);
            } else {
                print_table_subrow("├─", "Avg RX Speed", "(sampling...)");
                print_table_subrow("└─", "Avg TX Speed", "(sampling...)");
            }
        } else {
            print_table_subrow("├─", "Avg RX Speed", "(first sample)");
            print_table_subrow("└─", "Avg TX Speed", "(first sample)");
        }

        save_traffic_state(&current_stats);
    } else {
        print_table_subrow("├─", "Total RX", "N/A");
        print_table_subrow("├─", "Total TX", "N/A");
        print_table_subrow("├─", "Avg RX Speed", "N/A");
        print_table_subrow("└─", "Avg TX Speed", "N/A");
    }

    print_table_end();
}

/* Show SYSTEM module */
static void status_show_system(void) {
    int temp = get_cpu_temperature();
    char pid_path[PATH_MAX];
    struct stat st;
    char uptime_str[64];

    print_separator();
    print_table_header("SYSTEM");

    /* CPU Temperature */
    if (temp > 0) {
        char temp_str[32];
        if (temp >= THERMAL_TEMP_CRITICAL / 1000) {
            snprintf(temp_str, sizeof(temp_str), "%d°C [CRITICAL]", temp);
            print_table_subrow_with_color("├─", "CPU Temp", temp_str, COLOR_RED);
        } else if (temp >= THERMAL_TEMP_WARN / 1000) {
            snprintf(temp_str, sizeof(temp_str), "%d°C [WARN]", temp);
            print_table_subrow_with_color("├─", "CPU Temp", temp_str, COLOR_YELLOW);
        } else {
            snprintf(temp_str, sizeof(temp_str), "%d°C", temp);
            print_table_subrow_with_color("├─", "CPU Temp", temp_str, COLOR_GREEN);
        }
    } else {
        print_table_subrow("├─", "CPU Temp", "N/A");
    }

    /* Daemon Uptime */
    snprintf(pid_path, sizeof(pid_path), "%s/%s", ATP_DEFAULT_DIR, ATP_PID_FILE);
    if (stat(pid_path, &st) == 0) {
        time_t now = time(NULL);
        int elapsed = (int)(now - st.st_mtime);
        format_uptime_human(elapsed, uptime_str, sizeof(uptime_str));
        print_table_subrow("└─", "Daemon Uptime", uptime_str);
    } else {
        print_table_subrow("└─", "Daemon Uptime", "N/A");
    }

    print_table_end();
}

/* Show CONFIGURATION module (for --config flag) */
void status_show_config(atp_config_t *cfg) {
    char ports_str[64];
    char mark_str[64];

    printf("\n" COLOR_CYAN "=== ATP Configuration ===\n" COLOR_RESET "\n");

    /* Configuration */
    print_separator();
    print_table_header("CONFIGURATION");

    print_table_subrow("├─", "Proxy Mode", proxy_mode_to_string(cfg));
    print_table_subrow("├─", "Performance", cfg->performance_mode ? "ACTIVE" : "DISABLED");
    snprintf(ports_str, sizeof(ports_str), "TCP=%d, UDP=%d, REDIRECT=%d",
             cfg->tcp_port, cfg->udp_port, cfg->redirect_tcp_port);
    print_table_subrow("├─", "Ports", ports_str);
    print_table_subrow("├─", "IPv6", cfg->proxy_ipv6 ? "ENABLED" : "DISABLED");

    char dns_str[64];
    snprintf(dns_str, sizeof(dns_str), "%s (port %d)",
             cfg->dns_hijack ? "ENABLED" : "DISABLED", cfg->dns_port);
    print_table_subrow("├─", "DNS Hijack", dns_str);
    print_table_subrow_int("├─", "Table ID", cfg->table_id);
    snprintf(mark_str, sizeof(mark_str), "IPv4=0x%x, IPv6=0x%x", cfg->mark_value, cfg->mark_value6);
    print_table_subrow("└─", "Mark", mark_str);

    print_table_end();

    /* Interface Control */
    print_separator();
    print_table_header("INTERFACE CONTROL");

    char mobile_status[64];
    snprintf(mobile_status, sizeof(mobile_status), "%s → %s",
             cfg->mobile_iface, cfg->proxy_mobile ? "PROXIED" : "BYPASS");
    print_table_subrow("├─", "MOBILE", mobile_status);

    char wifi_status[64];
    snprintf(wifi_status, sizeof(wifi_status), "%s → %s",
             cfg->wifi_iface, cfg->proxy_wifi ? "PROXIED" : "BYPASS");
    print_table_subrow("├─", "WIFI", wifi_status);

    char hotspot_status[64];
    snprintf(hotspot_status, sizeof(hotspot_status), "%s → %s",
             cfg->hotspot_iface, cfg->proxy_hotspot ? "PROXIED" : "BYPASS");
    print_table_subrow("├─", "HOTSPOT", hotspot_status);

    char usb_status[64];
    snprintf(usb_status, sizeof(usb_status), "%s → %s",
             cfg->usb_iface, cfg->proxy_usb ? "PROXIED" : "BYPASS");
    print_table_subrow("└─", "USB", usb_status);

    print_table_end();

    /* Filters */
    print_separator();
    print_table_header("FILTERS");

    /* App Filter */
    if (cfg->app_proxy_enable) {
        char app_status[128];
        snprintf(app_status, sizeof(app_status), "ENABLED (%s mode)", cfg->app_proxy_mode);
        print_table_subrow("├─", "App Filter", app_status);
    } else {
        print_table_subrow("├─", "App Filter", "DISABLED");
    }

    /* MAC Filter */
    if (cfg->mac_filter_enable) {
        char mac_status[128];
        snprintf(mac_status, sizeof(mac_status), "ENABLED (%s mode)", cfg->mac_proxy_mode);
        print_table_subrow("├─", "MAC Filter", mac_status);
    } else {
        print_table_subrow("├─", "MAC Filter", "DISABLED");
    }

    /* CN IP Bypass */
    if (cfg->bypass_cn_ip) {
        print_table_subrow("└─", "CN IP Bypass", "ENABLED (ipset cnip)");
    } else {
        print_table_subrow("└─", "CN IP Bypass", "DISABLED");
    }

    print_table_end();
}

/* Main status show function */
void status_show(atp_config_t *cfg, service_ctx_t *svc, api_ctx_t *api) {
    (void)svc;
    (void)api;
    
    printf("\n" COLOR_CYAN "=== ATP Status ===\n" COLOR_RESET "\n");

    status_show_proxy_core();
    printf("\n");

    status_show_clash_mode(cfg);
    printf("\n");

    status_show_monitors();
    printf("\n");

    status_show_vpn();
    printf("\n");

    status_show_system();
}
