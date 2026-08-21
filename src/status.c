/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2025 ATP Project
 *
 * Status display
 */

#include "atpd_global.h"
#include "status.h"
#include "logger.h"
#include "utils.h"
#include "netlink.h"
#include "service.h"
#include "api.h"
#include "fcm_monitor.h"
#include "ui.h"
#include "perf_mode.h"
#include "atpd_context.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>

#define TRAFFIC_STATE_FILE "/data/adb/atp/run/traffic.state"
#define THERMAL_ZONE_BASE "/sys/class/thermal"
#define THERMAL_TEMP_WARN 75000
#define THERMAL_TEMP_CRITICAL 85000

static const char* proxy_mode_to_string(atp_config_t *cfg) {
    switch (cfg->network.proxy_mode) {
        case 0: return cfg->network.use_tproxy ? "TPROXY (auto)" : "REDIRECT (auto)";
        case 1: return "TPROXY (TCP+UDP)";
        case 2: return "REDIRECT (TCP only)";
        case 3: return "ENHANCE (TCP=REDIRECT, UDP=TPROXY)";
        default: return "UNKNOWN";
    }
}

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

static void status_show_proxy_core(service_ctx_t *svc) {
    int pid = service_get_pid(svc);
    char uptime_str[64];
    char mem_str[32];
    char cpu_str[16];
    char threads_str[16];
    char fds_str[16];
    char version_str[64];

    ui_table_begin();
    ui_table_header("PROXY CORE");

    if (pid <= 0) {
        ui_table_row_color("STATUS", "sing-box", COLOR_RED);
        ui_table_end();
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

    ui_table_row_color(ui_emoji_service(1), "sing-box", COLOR_GREEN);
    ui_table_subrow_int("├─", "PID", pid);
    ui_table_subrow("├─", "Uptime", uptime_str);
    ui_table_subrow("├─", "Memory", mem_str);
    ui_table_subrow("├─", "CPU", cpu_str);
    ui_table_subrow("├─", "Threads", threads_str);
    ui_table_subrow("├─", "FDs", fds_str);
    ui_table_subrow("└─", "Version", version_str);

    ui_table_end();
}

static void status_show_clash_mode(atp_config_t *cfg, api_ctx_t *api, service_ctx_t *svc) {
    char current_mode[64] = {0};

    ui_table_begin();
    ui_table_header("CLASH MODE");

    if (service_get_pid(svc) <= 0) {
        ui_table_row_color("MODE", "N/A (service stopped)", COLOR_YELLOW);
        ui_table_end();
        return;
    }

    if (api_get_mode_sync(api, current_mode, sizeof(current_mode)) == 0) {
        const char *color = COLOR_GREEN;
        if (strcmp(current_mode, "Rule") == 0) color = COLOR_CYAN;
        else if (strcmp(current_mode, "Global") == 0) color = COLOR_YELLOW;
        else if (strcmp(current_mode, "Google VPN") == 0) color = COLOR_GREEN;
        ui_table_row_color(ui_emoji_info(), current_mode, color);
    } else {
        ui_table_row_color(ui_emoji_info(), cfg->filter.user_clash_mode, COLOR_YELLOW);
        ui_table_warning("API unavailable, using cached value");
    }

    ui_table_end();
}

static void status_show_monitors(void) {
    time_t last_fcm = fcm_monitor_get_last_detection();
    time_t now = time(NULL);
    char fcm_status[64];

    ui_table_begin();
    ui_table_header("MONITORS");

    if (0) {
        ui_table_subrow_color("├─", "Netlink Monitor", "ACTIVE", COLOR_GREEN);
    } else {
        ui_table_subrow_color("├─", "Netlink Monitor", "INACTIVE", COLOR_RED);
    }

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
            ui_table_subrow_color("└─", "FCM Monitor", fcm_status, COLOR_GREEN);
        } else {
            ui_table_subrow_color("└─", "FCM Monitor", "ACTIVE (waiting for FCM)", COLOR_CYAN);
        }
    } else {
        ui_table_subrow_color("└─", "FCM Monitor", "INACTIVE", COLOR_RED);
    }

    ui_table_end();
}

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
                     stats->iface, &stats->rx_bytes, &stats->tx_bytes, (long*)&stats->timestamp);
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

    fprintf(fp, "%s %llu %llu %ld\n", stats->iface, stats->rx_bytes, stats->tx_bytes, (long)stats->timestamp);
    fclose(fp);
    return 0;
}

static void status_show_vpn(void) {
    char vpn_iface[IFNAMSIZ] = {0};
    unsigned long long rx_bytes = 0, tx_bytes = 0;
    char rx_str[32], tx_str[32];
    char rx_speed_str[32], tx_speed_str[32];
    iface_stats_t current_stats, prev_stats;
    int has_vpn = 0;

    ui_table_begin();
    ui_table_header("VPN STATUS");

    if (netlink_get_active_vpn(vpn_iface, sizeof(vpn_iface)) == 0 && vpn_iface[0]) {
        has_vpn = 1;
    }

    if (!has_vpn) {
        ui_table_row_color(ui_emoji_vpn(0), "DISCONNECTED", COLOR_RED);
        ui_table_end();
        return;
    }

    ui_table_subrow("├─", "Interface", vpn_iface);
    ui_table_subrow_color("├─", ui_emoji_vpn(1), "CONNECTED", COLOR_GREEN);

    if (get_iface_traffic(vpn_iface, &rx_bytes, &tx_bytes) == 0) {
        format_bytes(rx_str, sizeof(rx_str), rx_bytes);
        format_bytes(tx_str, sizeof(tx_str), tx_bytes);
        ui_table_subrow("├─", "📥 Total RX", rx_str);
        ui_table_subrow("├─", "📤 Total TX", tx_str);

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
                ui_table_subrow("├─", "📈 Avg RX Speed", speed_info);
                snprintf(speed_info, sizeof(speed_info), "%s (over %.0fs)", tx_speed_str, elapsed);
                ui_table_subrow("└─", "📉 Avg TX Speed", speed_info);
            } else {
                ui_table_subrow("├─", "📈 Avg RX Speed", "(sampling...)");
                ui_table_subrow("└─", "📉 Avg TX Speed", "(sampling...)");
            }
        } else {
            ui_table_subrow("├─", "📈 Avg RX Speed", "(first sample)");
            ui_table_subrow("└─", "📉 Avg TX Speed", "(first sample)");
        }
        save_traffic_state(&current_stats);
    } else {
        ui_table_subrow("├─", "📥 Total RX", "N/A");
        ui_table_subrow("├─", "📤 Total TX", "N/A");
        ui_table_subrow("├─", "📈 Avg RX Speed", "N/A");
        ui_table_subrow("└─", "📉 Avg TX Speed", "N/A");
    }

    ui_table_end();
}

static void status_show_engine_v2(void) {
    char buf[128];
    const char *stage = "IDLE";
    const char *color = COLOR_RED;
    const char *emoji = "x";

    ui_table_begin();
    ui_table_header("REACTOR ENGINE (v2.0)");

    switch (g_atpd_ctx.vpn_state) {
        case VPN_STATE_READY:
            stage = "READY";
            color = COLOR_GREEN;
            emoji = "⚡";
            break;
        case VPN_STATE_PREDICTING:
            stage = "PREDICTING";
            color = COLOR_CYAN;
            emoji = "~";
            break;
        case VPN_STATE_TEARDOWN:
            stage = "TEARDOWN";
            color = COLOR_RED;
            emoji = "!";
            break;
        default:
            stage = "IDLE";
            color = COLOR_RED;
            emoji = "x";
            break;
    }

    snprintf(buf, sizeof(buf), "%s  %s", emoji, stage);
    ui_table_row_color("State Machine", buf, color);

    int pipe_size = 0;
    if (g_atpd_ctx.xfrm_fd > 0) {
        pipe_size = fcntl(g_atpd_ctx.xfrm_fd, F_GETPIPE_SZ);
    }
    if (pipe_size > 0) {
        snprintf(buf, sizeof(buf), "%d KB", pipe_size / 1024);
    } else {
        snprintf(buf, sizeof(buf), "N/A");
    }
    ui_table_subrow("├─", "Pipe Size", buf);

    snprintf(buf, sizeof(buf), "0");
    ui_table_subrow_color("├─", "Backpressure", buf, COLOR_GREEN);

    snprintf(buf, sizeof(buf), "%llu bytes", (unsigned long long)g_atpd_ctx.splice_bytes_total);
    ui_table_subrow("├─", "Total Spliced", buf);

    const char *xfrm_status = "PENDING";
    const char *xfrm_color = COLOR_YELLOW;

    if (g_atpd_ctx.xfrm_if_id == 41) {
        xfrm_status = "LOCKED (IF_ID=41)";
        xfrm_color = COLOR_GREEN;
    } else if (g_atpd_ctx.vpn_state == VPN_STATE_READY) {
        xfrm_status = "LOCKED";
        xfrm_color = COLOR_GREEN;
    } else if (g_atpd_ctx.vpn_state == VPN_STATE_PREDICTING) {
        xfrm_status = "PREDICTING";
        xfrm_color = COLOR_CYAN;
    }

    snprintf(buf, sizeof(buf), "%s", xfrm_status);
    ui_table_subrow_color("└─", "XFRM Sync", buf, xfrm_color);

    ui_table_end();
}

static void status_show_system(void) {
    int temp = get_cpu_temperature();
    char pid_path[PATH_MAX];
    struct stat st;
    char uptime_str[64];

    ui_table_begin();
    ui_table_header("SYSTEM");

    if (temp > 0) {
        char temp_str[32];
        if (temp >= THERMAL_TEMP_CRITICAL / 1000) {
            snprintf(temp_str, sizeof(temp_str), "%d°C [CRITICAL]", temp);
            ui_table_subrow_color("├─", "🌡️ CPU Temp", temp_str, COLOR_RED);
        } else if (temp >= THERMAL_TEMP_WARN / 1000) {
            snprintf(temp_str, sizeof(temp_str), "%d°C [WARN]", temp);
            ui_table_subrow_color("├─", "🌡️ CPU Temp", temp_str, COLOR_YELLOW);
        } else {
            snprintf(temp_str, sizeof(temp_str), "%d°C", temp);
            ui_table_subrow_color("├─", "🌡️ CPU Temp", temp_str, COLOR_GREEN);
        }
    } else {
        ui_table_subrow("├─", "🌡️ CPU Temp", "N/A");
    }

    snprintf(pid_path, sizeof(pid_path), "%s/%s", ATP_DEFAULT_DIR, ATP_PID_FILE);
    if (stat(pid_path, &st) == 0) {
        time_t now = time(NULL);
        int elapsed = (int)(now - st.st_mtime);
        format_uptime_human(elapsed, uptime_str, sizeof(uptime_str));
        ui_table_subrow("└─", "⏱️ Daemon Uptime", uptime_str);
    } else {
        ui_table_subrow("└─", "⏱️ Daemon Uptime", "N/A");
    }

    ui_table_end();
}

void status_show_config(atp_config_t *cfg) {
    char ports_str[64];
    char mark_str[64];

    ui_title("ATP Configuration");

    ui_table_begin();
    ui_table_header("CONFIGURATION");
    ui_table_subrow("├─", "Proxy Mode", proxy_mode_to_string(cfg));
    ui_table_subrow("├─", "Performance", cfg->core.performance_mode ? "ACTIVE" : "DISABLED");
    snprintf(ports_str, sizeof(ports_str), "TCP=%d, UDP=%d, REDIRECT=%d", cfg->network.tcp_port, cfg->network.udp_port, cfg->network.redirect_tcp_port);
    ui_table_subrow("├─", "Ports", ports_str);
    ui_table_subrow("├─", "IPv6", cfg->network.proxy_ipv6 ? "ENABLED" : "DISABLED");
    char dns_str[64];
    snprintf(dns_str, sizeof(dns_str), "%s (port %d)", cfg->network.dns_hijack ? "ENABLED" : "DISABLED", cfg->network.dns_port);
    ui_table_subrow("├─", "DNS Hijack", dns_str);
    ui_table_subrow_int("├─", "Table ID", cfg->network.table_id);
    snprintf(mark_str, sizeof(mark_str), "IPv4=0x%x, IPv6=0x%x", cfg->network.mark_value, cfg->network.mark_value6);
    ui_table_subrow("└─", "Mark", mark_str);
    ui_table_end();

    ui_table_begin();
    ui_table_header("INTERFACE CONTROL");
    char mobile_status[64];
    snprintf(mobile_status, sizeof(mobile_status), "%s -> %s", cfg->interface.mobile_iface, cfg->interface.proxy_mobile ? "PROXIED" : "BYPASS");
    ui_table_subrow_emoji("├─", ui_emoji_mobile(), mobile_status);
    char wifi_status[64];
    snprintf(wifi_status, sizeof(wifi_status), "%s -> %s", cfg->interface.wifi_iface, cfg->interface.proxy_wifi ? "PROXIED" : "BYPASS");
    ui_table_subrow_emoji("├─", ui_emoji_wifi(1), wifi_status);
    char hotspot_status[64];
    snprintf(hotspot_status, sizeof(hotspot_status), "%s -> %s", cfg->interface.hotspot_iface, cfg->interface.proxy_hotspot ? "PROXIED" : "BYPASS");
    ui_table_subrow_emoji("├─", ui_emoji_hotspot(), hotspot_status);
    char usb_status[64];
    snprintf(usb_status, sizeof(usb_status), "%s -> %s", cfg->interface.usb_iface, cfg->interface.proxy_usb ? "PROXIED" : "BYPASS");
    ui_table_subrow_emoji("└─", ui_emoji_usb(), usb_status);
    ui_table_end();

    ui_table_begin();
    ui_table_header("FILTERS");
    if (cfg->filter.app_proxy_enable) {
        char app_status[128];
        snprintf(app_status, sizeof(app_status), "ENABLED (%s mode)", cfg->filter.app_proxy_mode);
        ui_table_subrow_emoji("├─", ui_emoji_mobile(), app_status);
    } else {
        ui_table_subrow_emoji("├─", ui_emoji_mobile(), "DISABLED");
    }
    if (cfg->filter.mac_filter_enable) {
        char mac_status[128];
        snprintf(mac_status, sizeof(mac_status), "ENABLED (%s mode)", cfg->filter.mac_proxy_mode);
        ui_table_subrow_emoji("├─", "🔢", mac_status);
    } else {
        ui_table_subrow_emoji("├─", "🔢", "DISABLED");
    }
    if (cfg->filter.bypass_cn_ip) {
        ui_table_subrow_emoji("└─", "🌏", "ENABLED (ipset cnip)");
    } else {
        ui_table_subrow_emoji("└─", "🌏", "DISABLED");
    }
    ui_table_end();
}

void status_show(atp_config_t *cfg, service_ctx_t *svc, api_ctx_t *api) {
    ui_title("ATP Status");

    status_show_proxy_core(svc);
    ui_blank();

    status_show_clash_mode(cfg, api, svc);
    ui_blank();

    ui_blank();

    status_show_monitors();
    ui_blank();

    status_show_vpn();
    ui_blank();

    status_show_engine_v2();
    ui_blank();

    status_show_system();
}
