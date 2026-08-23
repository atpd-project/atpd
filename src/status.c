/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * Status display - Pure eBPF Edition
 */

#include "atpd_global.h"
#include "status.h"
#include "logger.h"
#include "utils.h"
#include "netlink.h"
#include "service.h"
#include "api.h"
#include "ui.h"
#include "atpd_context.h"
#include "ebpf.h"
#include "ebpf_common.h"
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

static void status_show_proxy_core(service_ctx_t *svc) {
    int pid = service_get_pid(svc);
    if (pid <= 0) {
        pid = get_pid_by_name("sing-box");
    }

    char uptime_str[64] = "N/A";
    char mem_str[32] = "N/A";
    char cpu_str[16] = "0.0%";
    char threads_str[16] = "0";
    char fds_str[16] = "0";
    char version_str[64] = "unknown";

    ui_table_begin();
    ui_table_header("PROXY CORE");

    if (pid <= 0) {
        ui_table_row_color("STATUS", "sing-box (STOPPED)", COLOR_RED);
        ui_table_end();
        return;
    }

    long mem_kb = get_process_memory_kb(pid);
    double cpu = get_process_cpu_percent(pid);
    int threads = get_process_threads(pid);
    int fd_count = get_process_fd_count(pid);
    int uptime_sec = get_process_uptime_sec(pid);

    format_uptime_human(uptime_sec, uptime_str, sizeof(uptime_str));
    format_bytes(mem_str, sizeof(mem_str), (unsigned long long)mem_kb * 1024);
    snprintf(cpu_str, sizeof(cpu_str), "%.1f%%", cpu);
    snprintf(threads_str, sizeof(threads_str), "%d", threads);
    snprintf(fds_str, sizeof(fds_str), "%d", fd_count);

    /* 1. Fast path: Read version from Clash REST API /version via local socket (0.1ms) */
    char api_resp[512] = {0};
    char api_url[256];
    snprintf(api_url, sizeof(api_url), "http://%s:%d/version",
             g_config.api.host[0] ? g_config.api.host : "127.0.0.1",
             g_config.api.port > 0 ? g_config.api.port : 9090);
    if (api_get_sync(api_url, api_resp, sizeof(api_resp)) == 0 && api_resp[0]) {
        char *v = strstr(api_resp, "\"version\"");
        if (v) {
            char *colon = strchr(v, ':');
            if (colon) {
                char *q1 = strchr(colon, '"');
                if (q1) {
                    char *q2 = strchr(q1 + 1, '"');
                    if (q2) {
                        *q2 = '\0';
                        snprintf(version_str, sizeof(version_str), "%s", q1 + 1);
                    }
                }
            }
        }
    }

    /* 2. Fallback: Only if API is not yet active, query binary directly */
    if (strcmp(version_str, "unknown") == 0) {
        char bin_path[PATH_MAX] = {0};
        char exe_link[64];
        snprintf(exe_link, sizeof(exe_link), "/proc/%d/exe", pid);
        ssize_t rlen = readlink(exe_link, bin_path, sizeof(bin_path) - 1);
        if (rlen > 0) {
            bin_path[rlen] = '\0';
            get_binary_version(bin_path, version_str, sizeof(version_str));
        } else if (find_command_path("sing-box", bin_path, sizeof(bin_path)) == 0) {
            get_binary_version(bin_path, version_str, sizeof(version_str));
        }
    }

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

static void status_show_clash_mode(api_ctx_t *api, service_ctx_t *svc) {
    char current_mode[64] = {0};

    ui_table_begin();
    ui_table_header("CLASH MODE");

    int pid = service_get_pid(svc);
    if (pid <= 0) {
        pid = get_pid_by_name("sing-box");
    }

    if (pid <= 0) {
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
        ui_table_row_color(ui_emoji_info(), "Rule (Default)", COLOR_CYAN);
    }

    ui_table_end();
}

static void status_show_ebpf(void) {
    ebpf_probe_result_t probe;
    memset(&probe, 0, sizeof(probe));

    ui_table_begin();
    ui_table_header("PURE eBPF ENGINE");

    ui_table_subrow_color("├─", "Engine Mode", "Pure eBPF (Zero iptables)", COLOR_GREEN);
    ui_table_subrow("├─", "Data Path", "sing-box ebpf inbound");

    if (ebpf_probe_detailed(&probe) == 0 && probe.supported) {
        ui_table_subrow_color("├─", "eBPF Kernel", "AVAILABLE", COLOR_GREEN);
        char feat[128] = {0};
        int pos = 0;
        if (probe.has_cgroup_sock_addr) pos += snprintf(feat + pos, sizeof(feat) - pos, "cgroup_sock%s", (probe.has_sched_cls || probe.has_lpm_trie || probe.has_lru_hash) ? ", " : "");
        if (probe.has_sched_cls) pos += snprintf(feat + pos, sizeof(feat) - pos, "tc%s", (probe.has_lpm_trie || probe.has_lru_hash) ? ", " : "");
        if (probe.has_lpm_trie) pos += snprintf(feat + pos, sizeof(feat) - pos, "lpm_trie%s", probe.has_lru_hash ? ", " : "");
        if (probe.has_lru_hash) pos += snprintf(feat + pos, sizeof(feat) - pos, "lru_hash");
        ui_table_subrow("└─", "Capabilities", feat[0] ? feat : "Basic");
    } else {
        ui_table_subrow_color("├─", "eBPF Kernel", "UNSUPPORTED", COLOR_RED);
        ui_table_subrow("└─", "Capabilities", "None");
    }

    ui_table_end();
}

static int check_fcm_status(char *status_buf, size_t size, int *is_connected) {
    *is_connected = 0;
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
                            return 0;
                        }
                    }
                } else {
                    if (sscanf(line, "%*d: %*s %*[^:]:%x %x", &rem_port, &state) == 2) {
                        if (state == 1 && (rem_port == 0x146C || rem_port == 0x146D || rem_port == 0x146E)) {
                            snprintf(status_buf, size, "ACTIVE (mtalk [IPv6]:%d)", rem_port);
                            *is_connected = 1;
                            fclose(fp);
                            return 0;
                        }
                    }
                }
            }
        }
        fclose(fp);
    }

    snprintf(status_buf, size, "STANDBY (System Net Sensing)");
    return 0;
}

static void status_show_monitors(void) {
    ui_table_begin();
    ui_table_header("MONITORS & SENSING");

    char pid_path[PATH_MAX];
    snprintf(pid_path, sizeof(pid_path), "%s/%s",
             g_config.core.data_dir[0] ? g_config.core.data_dir : ATP_DEFAULT_DIR, ATP_PID_FILE);
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

static void get_traffic_state_file(char *buf, size_t size) {
    const char *base_dir = g_config.core.data_dir[0] ? g_config.core.data_dir : ".";
    snprintf(buf, size, "%s/%s", base_dir, TRAFFIC_STATE_FILE);
}

static int load_traffic_state(iface_stats_t *stats) {
    char state_file[PATH_MAX];
    get_traffic_state_file(state_file, sizeof(state_file));
    FILE *fp = fopen(state_file, "r");
    if (!fp) return -1;

    int ret = fscanf(fp, "%s %llu %llu %ld",
                     stats->iface, &stats->rx_bytes, &stats->tx_bytes, (long*)&stats->timestamp);
    fclose(fp);

    return (ret == 4 && stats->iface[0] != '\0') ? 0 : -1;
}

static int save_traffic_state(const iface_stats_t *stats) {
    char state_file[PATH_MAX];
    get_traffic_state_file(state_file, sizeof(state_file));
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

static void status_show_vpn(void) {
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
        ui_table_subrow("├─", "Secondary Tunnel", "None (Direct eBPF)");
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
    const char *stage = "STANDALONE";
    const char *color = COLOR_GREEN;
    const char *emoji = "⚡";

    ui_table_begin();
    ui_table_header("REACTOR ENGINE (v2.0)");

    char pid_path[PATH_MAX];
    snprintf(pid_path, sizeof(pid_path), "%s/%s",
             g_config.core.data_dir[0] ? g_config.core.data_dir : ATP_DEFAULT_DIR, ATP_PID_FILE);
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
                stage = "READY (Pure eBPF Active)";
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

static void status_show_system(void) {
    int temp = get_cpu_temperature();
    char pid_path[PATH_MAX];
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

    snprintf(pid_path, sizeof(pid_path), "%s/%s",
             g_config.core.data_dir[0] ? g_config.core.data_dir : ATP_DEFAULT_DIR, ATP_PID_FILE);
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
    (void)cfg;
    ui_title("ATP Pure eBPF Configuration");

    ui_table_begin();
    ui_table_header("CONFIGURATION");
    ui_table_subrow("├─", "Engine", "Pure eBPF (Zero iptables)");
    ui_table_subrow("├─", "Data Path", "sing-box ebpf inbound");
    ui_table_subrow("└─", "Supervisor", "Active (Circuit Breaker)");
    ui_table_end();
}

void status_show(atp_config_t *cfg, service_ctx_t *svc, api_ctx_t *api) {
    (void)cfg;
    ui_title("ATP Status (Pure eBPF Edition)");

    status_show_proxy_core(svc);
    ui_blank();

    status_show_clash_mode(api, svc);
    ui_blank();

    status_show_ebpf();
    ui_blank();

    status_show_monitors();
    ui_blank();

    status_show_vpn();
    ui_blank();

    status_show_engine_v2();
    ui_blank();

    status_show_system();
}
