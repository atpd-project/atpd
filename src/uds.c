/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * Unix Domain Socket - Runtime CLI command interface
 * Production Ready
 */

#include "reactor.h"
#include "logger.h"
#include "atpd_context.h"
#include "atpd_global.h"
#include "netlink.h"
#include "utils.h"
#include "version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <inttypes.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>

#define UDS_BUFFER_SIZE 4096
#define UDS_RESPONSE_SIZE 8192
#define UDS_BACKLOG 16
#define UDS_PATH_MAX 108

static int g_uds_fd = -1;
static char g_uds_path[UDS_PATH_MAX] = {0};
static reactor_t *g_uds_reactor = NULL;
static int g_uds_stop_requested = 0;
static int g_uds_reload_requested = 0;

/* ========== Safe Response Builder ========== */

static size_t append_response(char *buf, size_t size, size_t offset, const char *fmt, ...) {
    va_list args;
    int ret;

    if (offset >= size) {
        return size;
    }

    va_start(args, fmt);
    ret = vsnprintf(buf + offset, size - offset, fmt, args);
    va_end(args);

    if (ret < 0) {
        return offset;
    }

    if ((size_t)ret >= size - offset) {
        return size;
    }

    return offset + (size_t)ret;
}

/* ========== Response Helpers ========== */

static int send_response_all(int fd, const char *resp, size_t len) {
    if (!resp || len == 0) return 0;

    while (len > 0) {
        ssize_t n = send(fd, resp, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                LOG_DEBUG("UDS: send would block, partial response");
                return -1;
            }
            if (errno == EPIPE || errno == ECONNRESET) {
                LOG_DEBUG("UDS: client disconnected during send");
                return 0;
            }
            LOG_DEBUG("UDS: send failed: %s", strerror(errno));
            return 0;
        }
        if (n == 0) {
            return 0;
        }
        resp += n;
        len -= (size_t)n;
    }
    return 1;
}

static void send_string_all(int fd, const char *str) {
    if (str) {
        send_response_all(fd, str, strlen(str));
    }
}

static int check_client_uid(int fd) {
    struct ucred cred;
    socklen_t len = sizeof(cred);

    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0) {
        LOG_DEBUG("UDS: getsockopt SO_PEERCRED failed: %s", strerror(errno));
        return 0;
    }

    if (cred.uid != 0 && cred.uid != getuid()) {
        LOG_WARN("UDS: rejected connection from uid=%d", cred.uid);
        return 0;
    }

    LOG_DEBUG("UDS: accepted connection from uid=%d, pid=%d", cred.uid, cred.pid);
    return 1;
}

static void format_duration(uint64_t seconds, char *output, size_t size) {
    uint64_t days = seconds / 86400;
    unsigned hours = (unsigned)((seconds % 86400) / 3600);
    unsigned minutes = (unsigned)((seconds % 3600) / 60);
    unsigned secs = (unsigned)(seconds % 60);
    if (days) {
        snprintf(output, size, "%" PRIu64 "d %02u:%02u:%02u",
                 days, hours, minutes, secs);
    } else {
        snprintf(output, size, "%02u:%02u:%02u", hours, minutes, secs);
    }
}

static void read_process_status(pid_t pid, uint64_t *rss_kib,
                                unsigned *threads, unsigned *fds) {
    *rss_kib = 0;
    *threads = 0;
    *fds = 0;
    if (pid <= 0) return;

    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    FILE *fp = fopen(path, "r");
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            unsigned long long rss = 0;
            unsigned count = 0;
            if (sscanf(line, "VmRSS: %llu kB", &rss) == 1) *rss_kib = rss;
            if (sscanf(line, "Threads: %u", &count) == 1) *threads = count;
        }
        fclose(fp);
    }

    snprintf(path, sizeof(path), "/proc/%d/fd", pid);
    DIR *dir = opendir(path);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (entry->d_name[0] != '.') (*fds)++;
    }
    closedir(dir);
}

static int read_cpu_temperature(void) {
    DIR *dir = opendir("/sys/class/thermal");
    if (!dir) return -1;

    int temperature = -1;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (strncmp(entry->d_name, "thermal_zone", 12) != 0) continue;
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "/sys/class/thermal/%s/temp", entry->d_name);
        FILE *fp = fopen(path, "r");
        long millidegrees;
        if (fp && fscanf(fp, "%ld", &millidegrees) == 1 && millidegrees > 0) {
            temperature = (int)(millidegrees / 1000);
        }
        if (fp) fclose(fp);
        if (temperature >= 0) break;
    }
    closedir(dir);
    return temperature;
}

static void format_bytes(uint64_t bytes, char *output, size_t size) {
    const char *unit = "B";
    double value = (double)bytes;
    if (bytes >= 1024ULL * 1024 * 1024) {
        value /= 1024.0 * 1024 * 1024;
        unit = "GiB";
    } else if (bytes >= 1024ULL * 1024) {
        value /= 1024.0 * 1024;
        unit = "MiB";
    } else if (bytes >= 1024) {
        value /= 1024.0;
        unit = "KiB";
    }
    snprintf(output, size, "%.1f %s", value, unit);
}

static void format_rate(uint64_t bytes_per_sec, char *output, size_t size) {
    const char *unit = "bps";
    double value = (double)bytes_per_sec * 8.0;
    if (value >= 1024.0 * 1024 * 1024) {
        value /= 1024.0 * 1024 * 1024;
        unit = "Gbps";
    } else if (value >= 1024.0 * 1024) {
        value /= 1024.0 * 1024;
        unit = "Mbps";
    } else if (value >= 1024.0) {
        value /= 1024.0;
        unit = "Kbps";
    }
    snprintf(output, size, "%.1f %s", value, unit);
}

static uint64_t monotonic_age(const struct timespec *since) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec >= since->tv_sec ? (uint64_t)(now.tv_sec - since->tv_sec) : 0;
}

static size_t append_traffic(char *response, size_t size, size_t off,
                             const char *label, const vpn_traffic_t *traffic) {
    char rx[32];
    char tx[32];
    char rx_rate[32] = "sampling";
    char tx_rate[32] = "sampling";
    format_bytes(traffic->rx_bytes, rx, sizeof(rx));
    format_bytes(traffic->tx_bytes, tx, sizeof(tx));
    if (traffic->rate_ready) {
        format_rate(traffic->rx_bytes_per_sec, rx_rate, sizeof(rx_rate));
        format_rate(traffic->tx_bytes_per_sec, tx_rate, sizeof(tx_rate));
    }
    return append_response(response, size, off,
                           "  %s RX / TX       %s / %s\n"
                           "  %s rate          %s / %s\n",
                           label, rx, tx, label, rx_rate, tx_rate);
}

static const char *yes_no(int value) {
    return value ? "yes" : "no";
}

static void handle_status(int fd) {
    char response[UDS_RESPONSE_SIZE];
    size_t off = 0;

    uint64_t daemon_uptime = atpd_runtime_get_uptime();
    char daemon_uptime_text[32];
    format_duration(daemon_uptime, daemon_uptime_text, sizeof(daemon_uptime_text));

    vpn_state_t vpn_state = atomic_load(&g_atpd_ctx.vpn_state);
    int vpn_connected = vpn_state == VPN_STATE_READY;
    int vpn_stable = vpn_state == VPN_STATE_IDLE || vpn_state == VPN_STATE_READY;
    int policy_ok = !vpn_connected || g_atpd_ctx.hotspot_count == 0 ||
        (g_atpd_ctx.hotspot_ipv4_active == g_atpd_ctx.hotspot_count &&
         (!g_atpd_ctx.vpn_ipv6_default ||
          g_atpd_ctx.hotspot_ipv6_active == g_atpd_ctx.hotspot_count));
    int netlink_ok = netlink_get_fd() >= 0;
    int xfrm_ok = g_atpd_ctx.xfrm_fd >= 0;
    const reactor_stats_t *reactor_stats = reactor_get_stats(g_reactor);
    int healthy = g_atpd_ctx.runtime_state == ATPD_RUNTIME_STATE_RUNNING &&
        netlink_ok && xfrm_ok && reactor_stats && vpn_stable && policy_ok;
    const char *overall = healthy ? "HEALTHY" : "DEGRADED";

    off = append_response(response, sizeof(response), off,
                          "ATPD_STATUS %d\n", healthy ? 0 : 1);
    off = append_response(response, sizeof(response), off,
                          "ATPd %s                                      %s\n\n",
                          ATP_VERSION_STRING, overall);
    off = append_response(response, sizeof(response), off,
                          "Daemon\n"
                          "  State               %s\n"
                          "  PID / Uptime        %d / %s\n"
                          "  Backend             %s\n"
                          "  Config              %s\n\n",
                          atpd_runtime_state_string(g_atpd_ctx.runtime_state),
                          getpid(), daemon_uptime_text, g_config.network.backend,
                          g_atpd_ctx.config_path[0] ? g_atpd_ctx.config_path : "unknown");

    uint64_t sync_age = g_atpd_ctx.clash_last_sync &&
        (uint64_t)time(NULL) >= g_atpd_ctx.clash_last_sync
        ? (uint64_t)time(NULL) - g_atpd_ctx.clash_last_sync : 0;
    off = append_response(response, sizeof(response), off,
                          "Clash policy\n"
                          "  Configured mode     %s\n"
                          "  Desired mode        %s\n"
                          "  Applied mode        %s\n"
                          "  Last sync           %s",
                          g_config.filter.user_clash_mode,
                          g_atpd_ctx.clash_desired_mode[0]
                              ? g_atpd_ctx.clash_desired_mode : "unknown",
                          g_atpd_ctx.clash_applied_mode[0]
                              ? g_atpd_ctx.clash_applied_mode : "unknown",
                          g_atpd_ctx.clash_last_sync ? "" : "never\n");
    if (g_atpd_ctx.clash_last_sync) {
        off = append_response(response, sizeof(response), off,
                              "%" PRIu64 "s ago\n", sync_age);
    }
    off = append_response(response, sizeof(response), off,
                          "  Last error          %s\n\n",
                          g_atpd_ctx.clash_last_error[0]
                              ? g_atpd_ctx.clash_last_error : "none");

    off = append_response(response, sizeof(response), off,
                          "Monitors\n"
                          "  Netlink             %s\n"
                          "  XFRM listener       %s\n",
                          netlink_ok ? "ACTIVE" : "INACTIVE",
                          xfrm_ok ? "ACTIVE" : "INACTIVE");
    uint64_t fcm_age = g_atpd_ctx.fcm_last_seen &&
        (uint64_t)time(NULL) >= g_atpd_ctx.fcm_last_seen
        ? (uint64_t)time(NULL) - g_atpd_ctx.fcm_last_seen : 0;
    if (!g_atpd_ctx.fcm_monitor_active) {
        off = append_response(response, sizeof(response), off,
                              "  FCM                 INACTIVE%s",
                              g_atpd_ctx.fcm_last_seen ? " (last seen " : "\n\n");
        if (g_atpd_ctx.fcm_last_seen) {
            off = append_response(response, sizeof(response), off,
                                  "%" PRIu64 "s ago)\n\n", fcm_age);
        }
    } else if (g_atpd_ctx.fcm_last_seen) {
        off = append_response(response, sizeof(response), off,
                              "  FCM                 ACTIVE (seen %" PRIu64 "s ago)\n\n",
                              fcm_age);
    } else {
        off = append_response(response, sizeof(response), off,
                              "  FCM                 ACTIVE (waiting)\n\n");
    }

    const char *google_status = vpn_state == VPN_STATE_READY ? "CONNECTED" :
        vpn_state == VPN_STATE_PREDICTING ? "CONNECTING" :
        vpn_state == VPN_STATE_TEARDOWN ? "DISCONNECTING" : "DISCONNECTED";
    const char *other_status = g_atpd_ctx.other_vpn_iface[0] ? "CONNECTED" : "DISCONNECTED";
    off = append_response(response, sizeof(response), off,
                          "VPN\n"
                          "  Google VPN          %s\n"
                          "  Google interface    %s\n"
                          "  Other VPN           %s\n"
                          "  Other interface     %s\n"
                          "  Route table         %s",
                          google_status,
                          g_atpd_ctx.vpn_iface[0] ? g_atpd_ctx.vpn_iface : "none",
                          other_status,
                          g_atpd_ctx.other_vpn_iface[0] ? g_atpd_ctx.other_vpn_iface : "none",
                          g_atpd_ctx.vpn_route_table ? "" : "none\n");
    if (g_atpd_ctx.vpn_route_table) {
        off = append_response(response, sizeof(response), off, "%u\n",
                              g_atpd_ctx.vpn_route_table);
    }
    if (vpn_connected && g_atpd_ctx.google_vpn_traffic.sampled_at) {
        off = append_traffic(response, sizeof(response), off, "Google",
                             &g_atpd_ctx.google_vpn_traffic);
    }
    if (g_atpd_ctx.other_vpn_iface[0] &&
        g_atpd_ctx.other_vpn_traffic.sampled_at) {
        off = append_traffic(response, sizeof(response), off, "Other ",
                             &g_atpd_ctx.other_vpn_traffic);
    }
    off = append_response(response, sizeof(response), off,
                          "  IPv4 default route  %s\n"
                          "  IPv6 default route  %s\n\n",
                          yes_no(g_atpd_ctx.vpn_ipv4_default),
                          yes_no(g_atpd_ctx.vpn_ipv6_default));

    direct_wifi_state_t wifi_state = atomic_load(&g_atpd_ctx.direct_wifi_state);
    const char *xfrm_sync = vpn_state == VPN_STATE_PREDICTING ? "PREDICTING" :
        vpn_state == VPN_STATE_READY ? "LOCKED" : "PENDING";
    off = append_response(response, sizeof(response), off,
                          "State machines\n"
                          "  Reactor             %s\n"
                          "  VPN                 %s (%" PRIu64 "s, %" PRIu64 " transitions)\n"
                          "  XFRM sync           %s",
                          g_reactor && g_reactor->running ? "RUNNING" : "STOPPED",
                          vpn_state_string(vpn_state),
                          monotonic_age(&g_atpd_ctx.vpn_state_since),
                          g_atpd_ctx.vpn_transitions,
                          xfrm_sync);
    if (g_atpd_ctx.xfrm_if_id) {
        off = append_response(response, sizeof(response), off,
                              " (IF_ID=%u)\n", g_atpd_ctx.xfrm_if_id);
    } else {
        off = append_response(response, sizeof(response), off, "\n");
    }
    off = append_response(response, sizeof(response), off,
                          "  Direct Wi-Fi        %s (%" PRIu64 "s, %" PRIu64 " transitions)\n"
                          "  Current SSID        %s\n"
                          "  Direct SSID         %s\n"
                          "  Restore mode        %s\n",
                          direct_wifi_state_string(wifi_state),
                          monotonic_age(&g_atpd_ctx.direct_wifi_state_since),
                          g_atpd_ctx.direct_wifi_transitions,
                          g_atpd_ctx.current_wifi_ssid[0]
                              ? g_atpd_ctx.current_wifi_ssid : "none",
                          g_config.filter.direct_wifi_ssid[0]
                              ? g_config.filter.direct_wifi_ssid : "disabled",
                          g_atpd_ctx.direct_wifi_restore_mode[0]
                              ? g_atpd_ctx.direct_wifi_restore_mode
                              : g_config.filter.user_clash_mode);
    if (reactor_stats) {
        off = append_response(response, sizeof(response), off,
                              "  Events / Timers     %" PRIu64 " / %" PRIu64 "\n"
                              "  Handlers / Timers   %zu / %zu\n\n",
                              reactor_stats->events_processed,
                              reactor_stats->timers_fired,
                              reactor_stats->active_handlers,
                              reactor_stats->active_timers);
    } else {
        off = append_response(response, sizeof(response), off, "\n");
    }

    const char *ipv4_rule = g_atpd_ctx.hotspot_count == 0 ? "not installed" :
        g_atpd_ctx.hotspot_ipv4_active == g_atpd_ctx.hotspot_count ? "active" : "incomplete";
    const char *ipv6_rule = !g_atpd_ctx.vpn_ipv6_default ?
        "not installed (no VPN IPv6 route)" :
        g_atpd_ctx.hotspot_count == 0 ? "not installed" :
        g_atpd_ctx.hotspot_ipv6_active == g_atpd_ctx.hotspot_count ? "active" : "incomplete";
    uint64_t reconcile_age = g_atpd_ctx.policy_last_reconcile &&
        (uint64_t)time(NULL) >= g_atpd_ctx.policy_last_reconcile
        ? (uint64_t)time(NULL) - g_atpd_ctx.policy_last_reconcile : 0;
    char reconcile_text[32];
    if (g_atpd_ctx.policy_last_reconcile) {
        snprintf(reconcile_text, sizeof(reconcile_text), "%" PRIu64 "s ago",
                 reconcile_age);
    } else {
        snprintf(reconcile_text, sizeof(reconcile_text), "never");
    }
    off = append_response(response, sizeof(response), off,
                          "Hotspot policy\n"
                          "  Interfaces          %s\n"
                          "  IPv4 rule           %s (%u/%u)\n"
                          "  IPv6 rule           %s (%u/%u)\n"
                          "  Last reconcile      %s\n",
                          g_atpd_ctx.hotspot_ifaces[0] ? g_atpd_ctx.hotspot_ifaces : "none",
                          ipv4_rule, g_atpd_ctx.hotspot_ipv4_active, g_atpd_ctx.hotspot_count,
                          ipv6_rule, g_atpd_ctx.hotspot_ipv6_active, g_atpd_ctx.hotspot_count,
                          reconcile_text);

    int temperature = read_cpu_temperature();
    off = append_response(response, sizeof(response), off,
                          "\nSystem\n"
                          "  CPU temperature     %s",
                          temperature >= 0 ? "" : "unknown\n");
    if (temperature >= 0) {
        off = append_response(response, sizeof(response), off, "%d C\n", temperature);
    }
    off = append_response(response, sizeof(response), off,
                          "\nOverall               %s\n", overall);

    if (off >= sizeof(response)) {
        send_string_all(fd, "ERROR: response too large\n");
        return;
    }

    send_response_all(fd, response, off);
}

static void handle_core_status(int fd) {
    char response[UDS_RESPONSE_SIZE];
    size_t off = 0;
    pid_t pid = g_svc ? service_get_pid(g_svc) : -1;
    uint64_t rss_kib;
    unsigned threads;
    unsigned fds;
    read_process_status(pid, &rss_kib, &threads, &fds);
    double cpu = pid > 0 ? get_process_cpu_percent(pid) : 0.0;

    char version[64] = "unknown";
    char mode[32] = "unknown";
    int version_ok = 0;
    int mode_ok = 0;
    if (pid > 0) {
        api_ctx_t api;
        api_init(&api, &g_config);
        version_ok = api_get_version_sync(&api, version, sizeof(version)) == 0;
        mode_ok = api_get_mode_sync(&api, mode, sizeof(mode)) == 0;
        api_cleanup(&api);
    }
    if (version_ok && g_svc) {
        snprintf(g_svc->version, sizeof(g_svc->version), "%s", version);
    } else if (g_svc && g_svc->version[0]) {
        snprintf(version, sizeof(version), "%s", g_svc->version);
    }
    if (mode_ok) {
        snprintf(g_atpd_ctx.clash_applied_mode,
                 sizeof(g_atpd_ctx.clash_applied_mode), "%s", mode);
    } else if (g_atpd_ctx.clash_applied_mode[0]) {
        snprintf(mode, sizeof(mode), "%s", g_atpd_ctx.clash_applied_mode);
    }

    uint64_t uptime = g_svc && g_svc->started_at > 0 &&
        time(NULL) >= g_svc->started_at
        ? (uint64_t)(time(NULL) - g_svc->started_at) : 0;
    char uptime_text[32];
    format_duration(uptime, uptime_text, sizeof(uptime_text));

    int healthy = g_svc && g_svc->state == SERVICE_RUNNING &&
        service_is_healthy(g_svc) && version_ok && mode_ok;
    int status = healthy ? 0 : pid > 0 ? 1 : 2;
    const char *overall = healthy ? "HEALTHY" : pid > 0 ? "DEGRADED" : "STOPPED";

    off = append_response(response, sizeof(response), off,
                          "CORE_STATUS %d\n", status);
    off = append_response(response, sizeof(response), off,
                          "sing-box                                      %s\n\n"
                          "Core\n"
                          "  State               %s\n"
                          "  API                 %s\n"
                          "  Endpoint            %s\n"
                          "  PID / Uptime        %d / %s\n"
                          "  Version             %s\n"
                          "  Mode                %s\n"
                          "  eBPF                %s, %s\n"
                          "  Config              %s\n",
                          overall,
                          g_svc ? service_state_string(g_svc->state) : "UNKNOWN",
                          version_ok || mode_ok ? "REACHABLE" : "UNREACHABLE",
                          g_api_ctx.base_url[0] ? g_api_ctx.base_url : "unknown",
                          pid, pid > 0 ? uptime_text : "unknown",
                          version, mode,
                          g_svc && g_svc->ebpf_mode[0] ? g_svc->ebpf_mode : "unknown",
                          g_svc && g_svc->ebpf_network[0] ? g_svc->ebpf_network : "unknown",
                          g_svc ? g_svc->conf_path : "unknown");
    if (rss_kib) {
        off = append_response(response, sizeof(response), off,
                              "  RSS / Threads / FDs %.1f MiB / %u / %u\n",
                              (double)rss_kib / 1024.0, threads, fds);
    } else {
        off = append_response(response, sizeof(response), off,
                              "  RSS / Threads / FDs unknown\n");
    }
    off = append_response(response, sizeof(response), off,
                          "  CPU                 %.1f%%\n"
                          "  Restarts            %u\n"
                          "  Last error          %s\n\n"
                          "Overall               %s\n",
                          cpu,
                          g_svc ? g_svc->restart_count : 0,
                          g_svc && g_svc->last_error[0] ? g_svc->last_error : "none",
                          overall);

    if (off >= sizeof(response)) {
        send_string_all(fd, "ERROR: response too large\n");
        return;
    }
    send_response_all(fd, response, off);
}

static void core_restart_done(service_ctx_t *ctx, void *userdata) {
    (void)userdata;
    ctx->restart_count++;
    if (service_start_async(ctx) != 0) {
        LOG_ERROR("Core restart failed");
    }
}

static void handle_core_control(int fd, const char *action) {
    if (!g_svc) {
        send_string_all(fd, "CORE_CONTROL 1\nCore service unavailable\n");
        return;
    }

    int result;
    if (strcmp(action, "start") == 0) {
        result = service_start_async(g_svc);
    } else if (strcmp(action, "stop") == 0) {
        result = service_stop_async(g_svc, NULL, NULL);
    } else if ((g_svc->state == SERVICE_STOPPED ||
                g_svc->state == SERVICE_FAILED) && service_get_pid(g_svc) < 0) {
        g_svc->restart_count++;
        result = service_start_async(g_svc);
    } else {
        result = service_stop_async(g_svc, core_restart_done, NULL);
    }

    char response[96];
    if (result == 0) {
        snprintf(response, sizeof(response),
                 "CORE_CONTROL 0\nCore %s scheduled\n", action);
    } else {
        snprintf(response, sizeof(response),
                 "CORE_CONTROL 1\nCore %s failed: %.48s\n", action,
                 g_svc->last_error[0] ? g_svc->last_error : "unknown error");
    }
    send_string_all(fd, response);
}

static void handle_stop(int fd) {
    send_string_all(fd, "Stopping ATPd...\n");

    LOG_INFO("UDS: received stop command, initiating shutdown");
    atpd_runtime_state_transition(ATPD_RUNTIME_STATE_STOPPING);

    g_uds_stop_requested = 1;
}

static void handle_ping(int fd) {
    send_string_all(fd, "pong\n");
}

static void handle_reload(int fd) {
    send_string_all(fd, "Reloading ATPd...\n");
    g_uds_reload_requested = 1;
}

static void handle_version(int fd) {
    char response[UDS_RESPONSE_SIZE];
    size_t off = 0;

    off = append_response(response, sizeof(response), off,
                          "ATPd Version: %s\n", ATP_VERSION_STRING);
    off = append_response(response, sizeof(response), off,
                          "Git Commit: %s\n", ATP_COMMIT);

    if (off >= sizeof(response)) {
        send_string_all(fd, "ERROR: response too large\n");
        return;
    }

    send_response_all(fd, response, off);
}

static void handle_stats(int fd) {
    char response[UDS_RESPONSE_SIZE];
    size_t off = 0;

    off = append_response(response, sizeof(response), off,
                          "=== Statistics ===\n");
    off = append_response(response, sizeof(response), off,
                          "Events Processed: %" PRIu64 "\n",
                          g_atpd_ctx.stats.events_processed);
    off = append_response(response, sizeof(response), off,
                          "Timers Fired: %" PRIu64 "\n",
                          g_atpd_ctx.stats.timers_fired);
    off = append_response(response, sizeof(response), off,
                          "Signals Received: %" PRIu64 "\n",
                          g_atpd_ctx.stats.signals_received);
    off = append_response(response, sizeof(response), off,
                          "Errors: %" PRIu64 "\n",
                          g_atpd_ctx.stats.errors_total);

    if (off >= sizeof(response)) {
        send_string_all(fd, "ERROR: response too large\n");
        return;
    }

    send_response_all(fd, response, off);
}

static void handle_help(int fd) {
    const char *help =
        "Available commands:\n"
        "  status    - Show runtime status\n"
        "  core status  - Show sing-box core status\n"
        "  core start   - Start sing-box core\n"
        "  core stop    - Stop sing-box core\n"
        "  core restart - Restart sing-box core\n"
        "  stop      - Shutdown ATPd\n"
        "  reload    - Reload ATPd and sing-box configuration\n"
        "  ping      - Check if ATPd is alive\n"
        "  version   - Show version information\n"
        "  stats     - Show statistics\n"
        "  help      - Show this help\n";
    send_string_all(fd, help);
}

/* ========== Command Processing ========== */

static void process_command(int fd, const char *cmd, size_t cmd_len) {
    char buf[UDS_BUFFER_SIZE];

    if (cmd_len == 0) {
        return;
    }

    if (cmd_len >= sizeof(buf) - 1) {
        send_string_all(fd, "ERROR: command too long\n");
        return;
    }

    memcpy(buf, cmd, cmd_len);
    buf[cmd_len] = '\0';

    size_t len = strcspn(buf, "\r\n");
    while (len > 0 && (buf[len - 1] == ' ' || buf[len - 1] == '\t')) len--;
    buf[len] = '\0';

    if (buf[0] == '\0') {
        return;
    }

    if (strcmp(buf, "status") == 0) {
        handle_status(fd);
    } else if (strcmp(buf, "core status") == 0) {
        handle_core_status(fd);
    } else if (strcmp(buf, "core start") == 0) {
        handle_core_control(fd, "start");
    } else if (strcmp(buf, "core stop") == 0) {
        handle_core_control(fd, "stop");
    } else if (strcmp(buf, "core restart") == 0) {
        handle_core_control(fd, "restart");
    } else if (strcmp(buf, "stop") == 0) {
        handle_stop(fd);
    } else if (strcmp(buf, "reload") == 0) {
        handle_reload(fd);
    } else if (strcmp(buf, "ping") == 0) {
        handle_ping(fd);
    } else if (strcmp(buf, "version") == 0) {
        handle_version(fd);
    } else if (strcmp(buf, "stats") == 0) {
        handle_stats(fd);
    } else if (strcmp(buf, "help") == 0 || strcmp(buf, "?") == 0) {
        handle_help(fd);
    } else if (strlen(buf) > 0) {
        send_string_all(fd, "Unknown command. Type 'help' for available commands.\n");
    }
}

/* ========== UDS Client Callback ========== */

static void uds_client_cb(reactor_t *r, int fd, uint32_t events, void *userdata) {
    (void)r;
    (void)userdata;

    if (events & (REACTOR_EVENT_ERROR | REACTOR_EVENT_HANGUP)) {
        LOG_DEBUG("UDS: client disconnected (fd=%d)", fd);
        reactor_remove_fd(r, fd);
        close(fd);
        return;
    }

    if (!(events & REACTOR_EVENT_READ)) return;

    if (!check_client_uid(fd)) {
        reactor_remove_fd(r, fd);
        close(fd);
        return;
    }

    char buf[UDS_BUFFER_SIZE];
    size_t total_read = 0;

    while (total_read < sizeof(buf) - 1) {
        ssize_t n = read(fd, buf + total_read, sizeof(buf) - 1 - total_read);
        if (n > 0) {
            total_read += (size_t)n;
        } else if (n == 0) {
            LOG_DEBUG("UDS: client closed connection (fd=%d)", fd);
            reactor_remove_fd(r, fd);
            close(fd);
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            LOG_ERROR("UDS: read error: %s", strerror(errno));
            reactor_remove_fd(r, fd);
            close(fd);
            return;
        }
    }

    if (total_read == 0) {
        return;
    }

    if (total_read >= sizeof(buf) - 1) {
        LOG_WARN("UDS: oversized request, closing connection");
        reactor_remove_fd(r, fd);
        close(fd);
        return;
    }

    buf[total_read] = '\0';

    char *cmd_start = buf;
    char *cmd_end;

    while (*cmd_start) {
        cmd_end = strchr(cmd_start, '\n');
        if (cmd_end) {
            *cmd_end = '\0';
            process_command(fd, cmd_start, (size_t)(cmd_end - cmd_start));
            cmd_start = cmd_end + 1;
        } else {
            process_command(fd, cmd_start, strlen(cmd_start));
            break;
        }
    }
    reactor_remove_fd(r, fd);
    close(fd);
}

/* ========== UDS Accept Callback ========== */

static void uds_accept_cb(reactor_t *r, int listen_fd, uint32_t events, void *userdata) {
    (void)r;
    (void)userdata;

    if (!(events & REACTOR_EVENT_READ)) return;

    while (1) {
        int client_fd = accept4(listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            LOG_ERROR("UDS: accept failed: %s", strerror(errno));
            break;
        }

        LOG_DEBUG("UDS: client connected (fd=%d)", client_fd);
        if (reactor_add_fd(r, client_fd,
                           REACTOR_EVENT_READ | REACTOR_EVENT_EDGE,
                           uds_client_cb, NULL) != 0) {
            LOG_ERROR("UDS: failed to register client fd=%d", client_fd);
            close(client_fd);
        }
    }
}

/* ========== Init/Cleanup ========== */

int uds_init(reactor_t *r, const char *path) {
    struct sockaddr_un addr;
    size_t path_len;

    if (!r || !path) {
        return -1;
    }

    path_len = strlen(path);
    if (path_len >= sizeof(addr.sun_path)) {
        LOG_ERROR("UDS: socket path too long: %zu (max %zu)",
                  path_len, sizeof(addr.sun_path) - 1);
        return -1;
    }

    g_uds_reactor = r;
    g_uds_stop_requested = 0;
    g_uds_reload_requested = 0;

    strncpy(g_uds_path, path, UDS_PATH_MAX - 1);
    g_uds_path[UDS_PATH_MAX - 1] = '\0';

    unlink(path);

    g_uds_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (g_uds_fd < 0) {
        LOG_ERROR("UDS: socket failed: %s", strerror(errno));
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';

    if (bind(g_uds_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("UDS: bind to %s failed: %s", path, strerror(errno));
        close(g_uds_fd);
        g_uds_fd = -1;
        return -1;
    }

    if (chmod(path, 0600) < 0) {
        LOG_WARN("UDS: chmod 0600 failed: %s", strerror(errno));
    }

    if (listen(g_uds_fd, UDS_BACKLOG) < 0) {
        LOG_ERROR("UDS: listen failed: %s", strerror(errno));
        close(g_uds_fd);
        unlink(path);
        g_uds_fd = -1;
        return -1;
    }

    if (reactor_add_fd(r, g_uds_fd, REACTOR_EVENT_READ | REACTOR_EVENT_EDGE,
                       uds_accept_cb, NULL) != 0) {
        LOG_ERROR("UDS: reactor add failed");
        close(g_uds_fd);
        unlink(path);
        g_uds_fd = -1;
        return -1;
    }

    LOG_INFO("UDS: control socket created at %s (0600)", path);
    return 0;
}

void uds_cleanup(void) {
    if (g_uds_fd >= 0) {
        if (g_uds_reactor) {
            reactor_remove_fd(g_uds_reactor, g_uds_fd);
        }
        close(g_uds_fd);
        g_uds_fd = -1;
    }

    if (g_uds_path[0] != '\0') {
        unlink(g_uds_path);
        LOG_DEBUG("UDS: socket file removed: %s", g_uds_path);
        g_uds_path[0] = '\0';
    }

    g_uds_reactor = NULL;
    g_uds_stop_requested = 0;
    g_uds_reload_requested = 0;
}

int uds_get_fd(void) {
    return g_uds_fd;
}

int uds_stop_requested(void) {
    return g_uds_stop_requested;
}

int uds_reload_requested(void) {
    return g_uds_reload_requested;
}

void uds_clear_requests(void) {
    g_uds_stop_requested = 0;
    g_uds_reload_requested = 0;
}

static int uds_client_open(const char *path, const char *command) {
    if (!path || !command) return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof(addr.sun_path)) {
        close(fd);
        return -1;
    }
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }

    if (send_response_all(fd, command, strlen(command)) <= 0 ||
        send_response_all(fd, "\n", 1) <= 0) {
        close(fd);
        return -1;
    }
    shutdown(fd, SHUT_WR);
    return fd;
}

static int uds_client_status_request(const char *path, const char *command,
                                     const char *prefix, FILE *output) {
    if (!output) return -1;
    int fd = uds_client_open(path, command);
    if (fd < 0) return -1;

    char response[UDS_RESPONSE_SIZE + 1];
    size_t used = 0;
    ssize_t n;
    while (used < sizeof(response) - 1 &&
           (n = read(fd, response + used, sizeof(response) - 1 - used)) > 0) {
        used += (size_t)n;
    }
    close(fd);
    if (n < 0 || used == sizeof(response) - 1) return -1;
    response[used] = '\0';

    char *newline = strchr(response, '\n');
    size_t prefix_len = strlen(prefix);
    if (!newline || strncmp(response, prefix, prefix_len) != 0 ||
        response[prefix_len] != ' ') {
        return -1;
    }
    char *end;
    long status = strtol(response + prefix_len + 1, &end, 10);
    if (end != newline || status < 0 || status > 2) return -1;
    newline++;
    size_t body_len = used - (size_t)(newline - response);
    if (fwrite(newline, 1, body_len, output) != body_len) return -1;
    return (int)status;
}

int uds_client_status(const char *path, FILE *output) {
    return uds_client_status_request(path, "status", "ATPD_STATUS", output);
}

int uds_client_core_status(const char *path, FILE *output) {
    return uds_client_status_request(path, "core status",
                                     "CORE_STATUS", output);
}

int uds_client_core_control(const char *path, const char *command, FILE *output) {
    return uds_client_status_request(path, command, "CORE_CONTROL", output);
}
