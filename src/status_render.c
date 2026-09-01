/* Snapshot-only human status rendering. */

#include "status.h"

#include "ui.h"
#include "version.h"

#include <string.h>
#include <unistd.h>

static void format_uptime(int seconds, char *buf, size_t size) {
    if (seconds < 0) {
        snprintf(buf, size, "N/A");
        return;
    }
    int days = seconds / 86400;
    int hours = (seconds % 86400) / 3600;
    int mins = (seconds % 3600) / 60;
    int secs = seconds % 60;
    if (days > 0) snprintf(buf, size, "%dd %02d:%02d:%02d", days, hours, mins, secs);
    else if (hours > 0) snprintf(buf, size, "%dh %02dm %02ds", hours, mins, secs);
    else if (mins > 0) snprintf(buf, size, "%dm %02ds", mins, secs);
    else snprintf(buf, size, "%ds", secs);
}

static void format_kb(long kb, char *buf, size_t size) {
    if (kb < 0) snprintf(buf, size, "N/A");
    else if (kb >= 1024 * 1024) snprintf(buf, size, "%.2f GB", (double)kb / (1024 * 1024));
    else if (kb >= 1024) snprintf(buf, size, "%.2f MB", (double)kb / 1024);
    else snprintf(buf, size, "%ld KB", kb);
}

static void format_bytes(uint64_t bytes, char *buf, size_t size) {
    if (bytes >= 1024ull * 1024 * 1024) snprintf(buf, size, "%.2f GB", (double)bytes / (1024 * 1024 * 1024));
    else if (bytes >= 1024ull * 1024) snprintf(buf, size, "%.2f MB", (double)bytes / (1024 * 1024));
    else if (bytes >= 1024) snprintf(buf, size, "%.2f KB", (double)bytes / 1024);
    else snprintf(buf, size, "%llu B", (unsigned long long)bytes);
}

static void format_int(int value, char *buf, size_t size) {
    if (value < 0) snprintf(buf, size, "N/A");
    else snprintf(buf, size, "%d", value);
}

static const char *service_state_name(service_state_t state) {
    switch (state) {
        case SERVICE_STARTING: return "STARTING";
        case SERVICE_RUNNING: return "RUNNING";
        case SERVICE_FAILED: return "FAILED";
        case SERVICE_STOPPING: return "STOPPING";
        default: return "STOPPED";
    }
}

static void render_atpd(ui_render_ctx_t *ui, const status_snapshot_t *snapshot) {
    char uptime[64], rss[32], hwm[32], fds[16], threads[16];
    format_uptime(snapshot->atpd_uptime_sec, uptime, sizeof(uptime));
    format_kb(snapshot->atpd_rss_kb, rss, sizeof(rss));
    format_kb(snapshot->atpd_hwm_kb, hwm, sizeof(hwm));
    format_int(snapshot->atpd_fd_count, fds, sizeof(fds));
    format_int(snapshot->atpd_thread_count, threads, sizeof(threads));

    ui_table_begin(ui);
    ui_table_header(ui, "ATPD DAEMON");
    ui_table_row_color(ui, "State", snapshot->daemon_running ? "RUNNING" : "STOPPED",
                       snapshot->daemon_running ? COLOR_GREEN : COLOR_YELLOW);
    if (snapshot->daemon_running) ui_table_subrow_int(ui, "├─", "PID", snapshot->atpd_pid);
    ui_table_subrow(ui, "├─", "Uptime", uptime);
    ui_table_subrow(ui, "├─", "RSS", rss);
    ui_table_subrow(ui, "├─", "Peak RSS", hwm);
    ui_table_subrow(ui, "├─", "FDs", fds);
    ui_table_subrow(ui, "└─", "Threads", threads);
    ui_table_end(ui);
}

static void render_proxy(ui_render_ctx_t *ui, const status_snapshot_t *snapshot) {
    ui_table_begin(ui);
    ui_table_header(ui, "PROXY CORE");
    if (snapshot->singbox_pid <= 0) {
        ui_table_row_color(ui, "STATUS", "sing-box (STOPPED)", COLOR_RED);
        ui_table_end(ui);
        return;
    }

    char uptime[64], rss[32], hwm[32], cpu[32], threads[16], fds[16];
    format_uptime(snapshot->singbox_uptime_sec, uptime, sizeof(uptime));
    format_kb(snapshot->singbox_rss_kb, rss, sizeof(rss));
    format_kb(snapshot->singbox_hwm_kb, hwm, sizeof(hwm));
    if (snapshot->singbox_cpu_percent < 0) snprintf(cpu, sizeof(cpu), "N/A");
    else snprintf(cpu, sizeof(cpu), "%.1f%%", snapshot->singbox_cpu_percent);
    format_int(snapshot->singbox_thread_count, threads, sizeof(threads));
    format_int(snapshot->singbox_fd_count, fds, sizeof(fds));

    ui_table_row_color(ui, ui_emoji_service(ui, 1), "sing-box", COLOR_GREEN);
    ui_table_subrow_int(ui, "├─", "PID", snapshot->singbox_pid);
    ui_table_subrow(ui, "├─", "State", service_state_name(snapshot->singbox_state));
    ui_table_subrow(ui, "├─", "Uptime", uptime);
    ui_table_subrow(ui, "├─", "Memory", rss);
    ui_table_subrow(ui, "├─", "Peak Memory", hwm);
    ui_table_subrow(ui, "├─", "CPU", cpu);
    ui_table_subrow(ui, "├─", "Threads", threads);
    ui_table_subrow(ui, "├─", "Goroutines", "N/A (owner snapshot unavailable)");
    ui_table_subrow(ui, "├─", "FDs", fds);
    ui_table_subrow(ui, "└─", "Version", "N/A (owner snapshot unavailable)");
    ui_table_end(ui);
}

static void render_api(ui_render_ctx_t *ui, const status_snapshot_t *snapshot) {
    char api[64];
    snprintf(api, sizeof(api), "Native API (Port %d)", snapshot->api_port);
    ui_table_begin(ui);
    ui_table_header(ui, "NATIVE API & MODE");
    ui_table_subrow(ui, "├─", "API Engine", api);
    ui_table_subrow_color(ui, "└─", "Clash Mode", "N/A (owner snapshot unavailable)", COLOR_YELLOW);
    ui_table_end(ui);
}

static void render_monitors(ui_render_ctx_t *ui, const status_snapshot_t *snapshot) {
    ui_table_begin(ui);
    ui_table_header(ui, "MONITORS & SENSING");
    ui_table_subrow_color(ui, "├─", "Netlink Listener",
                          snapshot->netlink_listener_active ? "ACTIVE" : "INACTIVE",
                          snapshot->netlink_listener_active ? COLOR_GREEN : COLOR_YELLOW);
    ui_table_subrow_color(ui, "├─", "XFRM SA Listener",
                          snapshot->xfrm_listener_active ? "ACTIVE" : "INACTIVE",
                          snapshot->xfrm_listener_active ? COLOR_GREEN : COLOR_YELLOW);
    ui_table_subrow(ui, "└─", "FCM Push Sensing", "N/A (owner snapshot unavailable)");
    ui_table_end(ui);
}

static void render_vpn(ui_render_ctx_t *ui, const status_snapshot_t *snapshot) {
    ui_table_begin(ui);
    ui_table_header(ui, "VPN TUNNEL STATUS");
    if (snapshot->vpn.state != VPN_STATE_READY || !snapshot->vpn.iface[0]) {
        ui_table_row_color(ui, ui_emoji_info(ui), "STANDALONE / DIRECT", COLOR_GREEN);
        ui_table_subrow(ui, "└─", "Data Path", "sing-box ebpf inbound");
        ui_table_end(ui);
        return;
    }

    ui_table_row_color(ui, ui_emoji_vpn(ui, 1), "CONNECTED", COLOR_GREEN);
    ui_table_subrow(ui, "├─", "Interface", snapshot->vpn.iface);
    if (snapshot->traffic_available) {
        char rx[32], tx[32];
        format_bytes(snapshot->vpn_rx_bytes, rx, sizeof(rx));
        format_bytes(snapshot->vpn_tx_bytes, tx, sizeof(tx));
        ui_table_subrow(ui, "├─", "Total RX", rx);
        ui_table_subrow(ui, "└─", "Total TX", tx);
    } else {
        ui_table_subrow(ui, "├─", "Total RX", "N/A");
        ui_table_subrow(ui, "└─", "Total TX", "N/A");
    }
    ui_table_end(ui);
}

static void render_system(ui_render_ctx_t *ui, const status_snapshot_t *snapshot) {
    char temperature[32];
    if (snapshot->cpu_temperature_c < 0) snprintf(temperature, sizeof(temperature), "N/A");
    else snprintf(temperature, sizeof(temperature), "%d°C", snapshot->cpu_temperature_c);

    ui_table_begin(ui);
    ui_table_header(ui, "SYSTEM");
    ui_table_subrow(ui, "├─", "ATPD Version", atp_get_full_version());
    ui_table_subrow(ui, "└─", "CPU Temp", temperature);
    ui_table_end(ui);
}

void status_render_snapshot(FILE *out, bool no_color,
                            const status_snapshot_t *snapshot) {
    if (!snapshot) return;
    FILE *target = out ? out : stdout;
    bool color = !no_color && isatty(fileno(target));
    ui_render_ctx_t ui;
    ui_render_ctx_init(&ui, target, 0, color, snapshot->emoji_enabled);

    ui_title(&ui, "ATPD Status");
    render_atpd(&ui, snapshot);
    ui_blank(&ui);
    render_proxy(&ui, snapshot);
    ui_blank(&ui);
    render_api(&ui, snapshot);
    ui_blank(&ui);
    render_monitors(&ui, snapshot);
    ui_blank(&ui);
    render_vpn(&ui, snapshot);
    ui_blank(&ui);
    render_system(&ui, snapshot);
}
