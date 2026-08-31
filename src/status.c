/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * Status display
 */

#include "status.h"
#include "logger.h"
#include "utils.h"
#include "netlink.h"
#include "service.h"
#include "api.h"
#include "ui.h"
#include "atpd_context.h"
#include "version.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <arpa/inet.h>

#define THERMAL_ZONE_BASE "/sys/class/thermal"
#define THERMAL_TEMP_WARN 75000
#define THERMAL_TEMP_CRITICAL 85000

static void get_daemon_pid_path(const atp_config_t *cfg, char *path, size_t size) {
    const char *configured = cfg && cfg->core.pid_file[0] ?
        cfg->core.pid_file : ATP_PID_FILE;
    if (configured[0] == '/') {
        snprintf(path, size, "%s", configured);
    } else {
        snprintf(path, size, "%s/%s",
                 cfg && cfg->core.data_dir[0] ? cfg->core.data_dir : ".",
                 configured);
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
                    int val = atoi(temp_str);
                    if (val > 1000) temp = val / 1000;
                    else if (val > 0) temp = val;
                    if (temp > 0) {
                        fclose(fp);
                        closedir(dir);
                        return temp;
                    }
                }
                fclose(fp);
            }
        }
    }
    closedir(dir);
    return -1;
}

static void status_show_proxy_core(service_ctx_t *svc, api_ctx_t *api) {
    int pid = service_get_pid(svc);
    if (pid <= 0) {
        pid = get_pid_by_name("sing-box");
    }

    char uptime_str[64] = "N/A";
    char mem_str[32] = "N/A";
    char cpu_str[16] = "0.0%";
    char threads_str[16] = "0";
    char goroutines_str[16] = "N/A";
    char fds_str[16] = "0";
    char version_str[64] = "unknown";
    char connections_in_str[16] = "N/A";
    char connections_out_str[16] = "N/A";
    char uplink_str[32] = "N/A";
    char downlink_str[32] = "N/A";
    char uplink_total_str[32] = "N/A";
    char downlink_total_str[32] = "N/A";
    ui_table_begin();
    ui_table_header("PROXY CORE");

    if (pid <= 0) {
        ui_table_row_color("STATUS", "sing-box (STOPPED)", COLOR_RED);
        ui_table_end();
        return;
    }

    singbox_status_t core_status;
    memset(&core_status, 0, sizeof(core_status));
    int core_status_ok = api && api_get_status_sync(api, &core_status) == 0;

    double cpu = get_process_cpu_percent(pid);
    int threads = get_process_threads(pid);
    int fd_count = get_process_fd_count(pid);
    int uptime_sec = get_process_uptime_sec(pid);

    format_uptime_human(uptime_sec, uptime_str, sizeof(uptime_str));
    if (core_status_ok) {
        format_bytes(mem_str, sizeof(mem_str), core_status.memory);
        snprintf(connections_in_str, sizeof(connections_in_str), "%d", core_status.connections_in);
        snprintf(connections_out_str, sizeof(connections_out_str), "%d", core_status.connections_out);
        if (core_status.traffic_available) {
            format_bytes(uplink_str, sizeof(uplink_str), (unsigned long long)(core_status.uplink > 0 ? core_status.uplink : 0));
            format_bytes(downlink_str, sizeof(downlink_str), (unsigned long long)(core_status.downlink > 0 ? core_status.downlink : 0));
            format_bytes(uplink_total_str, sizeof(uplink_total_str), (unsigned long long)(core_status.uplink_total > 0 ? core_status.uplink_total : 0));
            format_bytes(downlink_total_str, sizeof(downlink_total_str), (unsigned long long)(core_status.downlink_total > 0 ? core_status.downlink_total : 0));
        }
    } else snprintf(mem_str, sizeof(mem_str), "N/A");
    snprintf(cpu_str, sizeof(cpu_str), "%.1f%%", cpu);
    snprintf(threads_str, sizeof(threads_str), "%d", threads);
    snprintf(fds_str, sizeof(fds_str), "%d", fd_count);

    /* All dashboard runtime values come from one SubscribeStatus snapshot. */
    if (core_status_ok) {
        snprintf(goroutines_str, sizeof(goroutines_str), "%d", core_status.goroutines);
    } else {
        snprintf(goroutines_str, sizeof(goroutines_str), "N/A");
    }

    /* Version is authoritative only when returned by StartedService.GetVersion. */
    if (!api || api_get_version_sync(api, version_str, sizeof(version_str)) != 0 || !version_str[0]) {
        snprintf(version_str, sizeof(version_str), "unknown");
    }

    ui_table_row_color(ui_emoji_service(1), "sing-box", COLOR_GREEN);
    ui_table_subrow_int("├─", "PID", pid);
    ui_table_subrow("├─", "Uptime", uptime_str);
    ui_table_subrow("├─", "Memory", mem_str);
    ui_table_subrow("├─", "CPU", cpu_str);
    ui_table_subrow("├─", "Threads", threads_str);
    ui_table_subrow("├─", "Goroutines", goroutines_str);
    ui_table_subrow("├─", "Connections In", connections_in_str);
    ui_table_subrow("├─", "Connections Out", connections_out_str);
    ui_table_subrow("├─", "Uplink", uplink_str);
    ui_table_subrow("├─", "Downlink", downlink_str);
    ui_table_subrow("├─", "Uplink Total", uplink_total_str);
    ui_table_subrow("├─", "Downlink Total", downlink_total_str);
    ui_table_subrow("├─", "FDs", fds_str);
    ui_table_subrow("└─", "Version", version_str);

    ui_table_end();
}

static void status_show_proxy_mode(api_ctx_t *api, service_ctx_t *svc, atp_config_t *cfg) {
    char current_mode[64] = {0};

    ui_table_begin();
    ui_table_header("NATIVE API & MODE");

    int pid = service_get_pid(svc);
    if (pid <= 0) {
        pid = get_pid_by_name("sing-box");
    }

    if (pid <= 0) {
        ui_table_row_color("STATUS", "N/A (service stopped)", COLOR_YELLOW);
        ui_table_end();
        return;
    }

    int port = (api && api->native_ctx.port > 0) ? api->native_ctx.port :
               (cfg && cfg->api.port > 0 ? cfg->api.port : DEFAULT_API_PORT);

    char api_info[128];
    snprintf(api_info, sizeof(api_info), "Native API (Port %d)", port);
    ui_table_subrow_color("├─", "API Engine", api_info, COLOR_GREEN);

    if (api && api_get_mode_sync(api, current_mode, sizeof(current_mode)) == 0 && current_mode[0]) {
        const char *color = COLOR_GREEN;
        if (strcmp(current_mode, "Rule") == 0) color = COLOR_CYAN;
        else if (strcmp(current_mode, "Global") == 0) color = COLOR_YELLOW;
        else if (strcmp(current_mode, "Google VPN") == 0) color = COLOR_GREEN;
        char mode_display[128];
        snprintf(mode_display, sizeof(mode_display), "%s", current_mode);
        ui_table_subrow_color("└─", "Clash Mode", mode_display, color);
    } else {
        ui_table_subrow_color("└─", "Clash Mode", "N/A (service unavailable)", COLOR_YELLOW);
    }

    ui_table_end();
}

static int check_fcm_status(char *status_buf, size_t size, int *is_connected) {
    *is_connected = 0;

    static char s_cached_status[64] = "STANDBY (System Net Sensing)";
    static int s_cached_conn = 0;
    static time_t s_last_check = 0;
    time_t now = time(NULL);

    if (s_last_check != 0 && (now - s_last_check) < 2) {
        *is_connected = s_cached_conn;
        snprintf(status_buf, size, "%s", s_cached_status);
        return 0;
    }

    const char *paths[2] = { "/proc/net/tcp", "/proc/net/tcp6" };

    for (int f = 0; f < 2; f++) {
        FILE *fp = fopen(paths[f], "r");
        if (!fp) continue;

        char line[512];
        if (fgets(line, sizeof(line), fp)) {
            while (fgets(line, sizeof(line), fp)) {
                unsigned int rem_ip = 0, rem_port = 0, state = 0;
                if (f == 0) {
                    if (sscanf(line, "%*d: %*x:%*x %x:%x %x", &rem_ip, &rem_port, &state) == 3) {
                        if (state == 1 && (rem_port == 0x146C || rem_port == 0x146D || rem_port == 0x146E)) {
                            struct in_addr in;
                            in.s_addr = rem_ip;
                            char ip_str[INET_ADDRSTRLEN];
                            inet_ntop(AF_INET, &in, ip_str, sizeof(ip_str));
                            snprintf(status_buf, size, "ACTIVE (mtalk %s:%d)", ip_str, rem_port);
                            *is_connected = 1;
                            fclose(fp);
                            snprintf(s_cached_status, sizeof(s_cached_status), "%s", status_buf);
                            s_cached_conn = 1;
                            s_last_check = now;
                            return 0;
                        }
                    }
                } else {
                    if (sscanf(line, "%*d: %*s %*[^:]:%x %x", &rem_port, &state) == 2) {
                        if (state == 1 && (rem_port == 0x146C || rem_port == 0x146D || rem_port == 0x146E)) {
                            snprintf(status_buf, size, "ACTIVE (mtalk [IPv6]:%d)", rem_port);
                            *is_connected = 1;
                            fclose(fp);
                            snprintf(s_cached_status, sizeof(s_cached_status), "%s", status_buf);
                            s_cached_conn = 1;
                            s_last_check = now;
                            return 0;
                        }
                    }
                }
            }
        }
        fclose(fp);
    }

    snprintf(status_buf, size, "STANDBY (System Net Sensing)");
    snprintf(s_cached_status, sizeof(s_cached_status), "%s", status_buf);
    s_cached_conn = 0;
    s_last_check = now;
    return 0;
}

static void status_show_monitors(const atp_config_t *cfg) {
    ui_table_begin();
    ui_table_header("MONITORS & SENSING");

    char pid_path[PATH_MAX];
    get_daemon_pid_path(cfg, pid_path, sizeof(pid_path));
    int daemon_running = (access(pid_path, F_OK) == 0);

    if (daemon_running || netlink_get_fd() >= 0) {
        ui_table_subrow_color("├─", "Netlink Listener", "ACTIVE (Link / Route)", COLOR_GREEN);
        ui_table_subrow_color("├─", "XFRM SA Listener", "ACTIVE (IPsec Sensing)", COLOR_GREEN);
    } else {
        ui_table_subrow_color("├─", "Netlink Listener", "READY (Kernel Interface)", COLOR_CYAN);
        ui_table_subrow_color("├─", "XFRM SA Listener", "READY (IPsec SA Trigger)", COLOR_CYAN);
    }

    char fcm_status[64];
    int fcm_active = 0;
    check_fcm_status(fcm_status, sizeof(fcm_status), &fcm_active);
    ui_table_subrow_color("└─", "FCM Push Sensing", fcm_status, fcm_active ? COLOR_GREEN : COLOR_CYAN);

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

    if (!fgets(line, sizeof(line), fp)) { fclose(fp); return -1; }
    if (!fgets(line, sizeof(line), fp)) { fclose(fp); return -1; }

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

static void get_traffic_state_file(const atp_config_t *cfg, char *buf, size_t size) {
    const char *base_dir = cfg && cfg->core.data_dir[0] ? cfg->core.data_dir : ".";
    snprintf(buf, size, "%s/%s", base_dir, TRAFFIC_STATE_FILE);
}

static int load_traffic_state(const atp_config_t *cfg, iface_stats_t *stats) {
    char state_file[PATH_MAX];
    get_traffic_state_file(cfg, state_file, sizeof(state_file));
    FILE *fp = fopen(state_file, "r");
    if (!fp) return -1;

    int ret = fscanf(fp, "%s %llu %llu %ld",
                     stats->iface, &stats->rx_bytes, &stats->tx_bytes, (long*)&stats->timestamp);
    fclose(fp);

    return (ret == 4 && stats->iface[0] != '\0') ? 0 : -1;
}

static int save_traffic_state(const atp_config_t *cfg, const iface_stats_t *stats) {
    char state_file[PATH_MAX];
    get_traffic_state_file(cfg, state_file, sizeof(state_file));
    char dir[PATH_MAX];
    strncpy(dir, state_file, sizeof(dir) - 1);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        mkdir_recursive(dir, 0755);
    }

    FILE *fp = fopen(state_file, "w");
    if (!fp) return -1;

    fprintf(fp, "%s %llu %llu %ld\n", stats->iface, stats->rx_bytes, stats->tx_bytes, (long)stats->timestamp);
    fclose(fp);
    return 0;
}

static void status_show_vpn(const atp_config_t *cfg) {
    char vpn_iface[IFNAMSIZ] = {0};
    unsigned long long rx_bytes = 0, tx_bytes = 0;
    char rx_str[32], tx_str[32];
    char rx_speed_str[32], tx_speed_str[32];
    iface_stats_t current_stats, prev_stats;
    int has_vpn = 0;

    ui_table_begin();
    ui_table_header("VPN TUNNEL STATUS");

    if (netlink_get_active_vpn(vpn_iface, sizeof(vpn_iface)) == 0 && vpn_iface[0]) {
        has_vpn = 1;
    }

    if (!has_vpn) {
        ui_table_row_color(ui_emoji_info(), "STANDALONE / DIRECT", COLOR_GREEN);
        ui_table_subrow("├─", "Secondary Tunnel", "None (sing-box datapath)");
        ui_table_subrow("└─", "Data Path", "cgroup socket interception");
        ui_table_end();
        return;
    }

    const char *label = netlink_get_vpn_type_label(vpn_iface);
    char iface_display[64];
    snprintf(iface_display, sizeof(iface_display), "%s (%s)", vpn_iface, label);

    ui_table_row_color(ui_emoji_vpn(1), "CONNECTED (Secondary Tunnel)", COLOR_GREEN);
    ui_table_subrow("├─", "Interface", iface_display);

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

        if (load_traffic_state(cfg, &prev_stats) == 0 &&
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
        save_traffic_state(cfg, &current_stats);
    } else {
        ui_table_subrow("├─", "📥 Total RX", "N/A");
        ui_table_subrow("├─", "📤 Total TX", "N/A");
        ui_table_subrow("├─", "📈 Avg RX Speed", "N/A");
        ui_table_subrow("└─", "📉 Avg TX Speed", "N/A");
    }

    ui_table_end();
}

static void status_show_engine_v2(const atp_config_t *cfg) {
    char buf[128];
    const char *stage = "STANDALONE";
    const char *color = COLOR_GREEN;
    const char *emoji = "⚡";

    ui_table_begin();
    ui_table_header("REACTOR ENGINE (v2.0)");

    char pid_path[PATH_MAX];
    get_daemon_pid_path(cfg, pid_path, sizeof(pid_path));
    FILE *fp_pid = fopen(pid_path, "r");
    int daemon_pid = 0;
    int daemon_alive = 0;
    if (fp_pid) {
        if (fscanf(fp_pid, "%d", &daemon_pid) == 1 && daemon_pid > 0 && kill(daemon_pid, 0) == 0) {
            daemon_alive = 1;
        }
        fclose(fp_pid);
    }

    vpn_state_t vpn_st = (vpn_state_t)atomic_load(&g_atpd_ctx.vpn_state);

    if (daemon_alive) {
        switch (vpn_st) {
            case VPN_STATE_READY:
                stage = "READY (Tunnel Active)";
                color = COLOR_GREEN;
                emoji = "⚡";
                break;
            case VPN_STATE_PREDICTING:
                stage = "PREDICTING (Transition)";
                color = COLOR_CYAN;
                emoji = "~";
                break;
            case VPN_STATE_TEARDOWN:
                stage = "TEARDOWN";
                color = COLOR_YELLOW;
                emoji = "!";
                break;
            default:
                stage = "READY (Supervisor Active)";
                color = COLOR_GREEN;
                emoji = "⚡";
                break;
        }
    } else {
        stage = "STANDALONE (CLI Query)";
        color = COLOR_GREEN;
        emoji = "⚡";
    }

    snprintf(buf, sizeof(buf), "%s  %s", emoji, stage);
    ui_table_row_color("State Machine", buf, color);

    const char *xfrm_status = "IDLE (Direct Routing)";
    const char *xfrm_color = COLOR_GREEN;

    if (g_atpd_ctx.xfrm_if_id == 41) {
        xfrm_status = "LOCKED (IF_ID=41)";
        xfrm_color = COLOR_GREEN;
    } else if (vpn_st == VPN_STATE_READY) {
        xfrm_status = "LOCKED";
        xfrm_color = COLOR_GREEN;
    } else if (vpn_st == VPN_STATE_PREDICTING) {
        xfrm_status = "PREDICTING";
        xfrm_color = COLOR_CYAN;
    }

    snprintf(buf, sizeof(buf), "%s", xfrm_status);
    ui_table_subrow_color("└─", "XFRM Sync", buf, xfrm_color);

    ui_table_end();
}

static void status_show_system(const atp_config_t *cfg) {
    int temp = get_cpu_temperature();
    char pid_path[PATH_MAX];
    char uptime_str[64];

    ui_table_begin();
    ui_table_header("SYSTEM");
    ui_table_subrow("├─", "ATPD Version", atp_get_full_version());

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

    get_daemon_pid_path(cfg, pid_path, sizeof(pid_path));
    FILE *fp_pid = fopen(pid_path, "r");
    int daemon_pid = 0;
    if (fp_pid) {
        if (fscanf(fp_pid, "%d", &daemon_pid) == 1 && daemon_pid > 0 && kill(daemon_pid, 0) == 0) {
            int elapsed = get_process_uptime_sec(daemon_pid);
            format_uptime_human(elapsed, uptime_str, sizeof(uptime_str));
            ui_table_subrow("└─", "⏱️ Daemon Uptime", uptime_str);
        } else {
            ui_table_subrow("└─", "⏱️ Daemon Uptime", "N/A (Daemon stopped)");
        }
        fclose(fp_pid);
    } else {
        ui_table_subrow("└─", "⏱️ Daemon Uptime", "N/A (Daemon stopped)");
    }

    ui_table_end();
}

void status_show_config(atp_config_t *cfg) {
    ui_set_emoji_enabled(cfg ? cfg->core.ui_emoji_enabled : 1);
    ui_title("ATPD Configuration");

    ui_table_begin();
    ui_table_header("CONFIGURATION");
    ui_table_subrow("├─", "Engine", "ATPD control plane");
    ui_table_subrow("├─", "Data Path", "sing-box ebpf inbound");
    ui_table_subrow("└─", "Supervisor", "Active (Circuit Breaker)");
    ui_table_end();
}

void status_show(atp_config_t *cfg, service_ctx_t *svc, api_ctx_t *api) {
    ui_set_emoji_enabled(cfg ? cfg->core.ui_emoji_enabled : 1);
    ui_title("ATPD Status");

    status_show_proxy_core(svc, api);
    ui_blank();

    status_show_proxy_mode(api, svc, cfg);
    ui_blank();

    status_show_monitors(cfg);
    ui_blank();

    status_show_vpn(cfg);
    ui_blank();

    status_show_engine_v2(cfg);
    ui_blank();

    status_show_system(cfg);
}
