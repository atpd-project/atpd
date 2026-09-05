#include "status.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static char *render_with_pid(pid_t pid) {
    status_snapshot_t snapshot = {0};
    snapshot.emoji_enabled = true;
    snapshot.daemon_running = true;
    snapshot.api_port = 9080;
    snprintf(snapshot.kernel_release, sizeof(snapshot.kernel_release), "6.1.0-test");
    snapshot.atpd_pid = pid;
    snapshot.atpd_uptime_sec = 5;
    snapshot.atpd_fd_count = 7;
    snapshot.atpd_thread_count = 1;
    snapshot.atpd_rss_kb = 1024;
    snapshot.atpd_hwm_kb = 1280;
    snapshot.singbox_pid = -1;
    snapshot.cpu_temperature_c = -1;

    char *buffer = NULL;
    size_t size = 0;
    FILE *stream = open_memstream(&buffer, &size);
    assert(stream != NULL);
    status_render_snapshot(stream, true, &snapshot);
    if (fclose(stream) != 0) abort();
    assert(size > 0);
    return buffer;
}

static char *render_with_native_api(bool traffic_available) {
    status_snapshot_t snapshot = {0};
    snapshot.api_port = 9080;
    snapshot.singbox_pid = 30303;
    snapshot.singbox_state = SERVICE_RUNNING;
    snapshot.singbox_uptime_sec = 10;
    snapshot.singbox_fd_count = 8;
    snapshot.singbox_thread_count = 4;
    snapshot.singbox_rss_kb = 2048;
    snapshot.singbox_hwm_kb = 4096;
    snapshot.native_api.valid = true;
    snapshot.native_api.version_valid = true;
    snapshot.native_api.clash_mode_valid = true;
    snapshot.native_api.status.goroutines = 17;
    snapshot.native_api.status.traffic_available = traffic_available;
    snprintf(snapshot.native_api.version, sizeof(snapshot.native_api.version), "1.12.0");
    snprintf(snapshot.native_api.clash_mode, sizeof(snapshot.native_api.clash_mode), "Rule");

    char *buffer = NULL;
    size_t size = 0;
    FILE *stream = open_memstream(&buffer, &size);
    assert(stream != NULL);
    status_render_snapshot(stream, true, &snapshot);
    if (fclose(stream) != 0) abort();
    assert(size > 0);
    return buffer;
}

int main(void) {
    char *first = render_with_pid(10101);
    char *second = render_with_pid(20202);

    assert(strstr(first, "\033[") == NULL);
    assert(strstr(first, "10101") != NULL);
    assert(strstr(first, "20202") == NULL);
    assert(strstr(second, "20202") != NULL);
    assert(strstr(second, "10101") == NULL);
    assert(strstr(first, "Peak RSS") != NULL);
    assert(strstr(first, "6.1.0-test") != NULL);

    status_snapshot_t summary_snapshot = {0};
    summary_snapshot.daemon_running = true;
    summary_snapshot.atpd_pid = 10101;
    summary_snapshot.atpd_uptime_sec = 2;
    summary_snapshot.atpd_rss_kb = 1024;
    summary_snapshot.singbox_pid = 30303;
    summary_snapshot.singbox_state = SERVICE_RUNNING;
    summary_snapshot.singbox_uptime_sec = 1;
    summary_snapshot.singbox_rss_kb = 2048;
    snprintf(summary_snapshot.kernel_release, sizeof(summary_snapshot.kernel_release), "6.1.0-test");
    char *summary = NULL;
    size_t summary_size = 0;
    FILE *summary_stream = open_memstream(&summary, &summary_size);
    assert(summary_stream != NULL);
    status_render_summary(summary_stream, &summary_snapshot);
    assert(fclose(summary_stream) == 0);
    assert(strstr(summary, "ATPD:      RUNNING (PID: 10101)") != NULL);
    assert(strstr(summary, "sing-box:  RUNNING (PID: 30303)") != NULL);
    assert(strstr(summary, "Kernel:    6.1.0-test") != NULL);
    assert(strstr(summary, "Data path: sing-box ebpf inbound") != NULL);

    char *active = render_with_native_api(true);
    char *standby = render_with_native_api(false);
    assert(strstr(active, "Goroutines") != NULL);
    assert(strstr(active, "17") != NULL);
    assert(strstr(active, "1.12.0") != NULL);
    assert(strstr(active, "Rule") != NULL);
    assert(strstr(active, "ACTIVE (Native API Traffic)") != NULL);
    assert(strstr(standby, "STANDBY (Native API Traffic)") != NULL);
    assert(strstr(active, "owner snapshot unavailable") == NULL);

    free(first);
    free(second);
    free(summary);
    free(active);
    free(standby);
    return 0;
}
