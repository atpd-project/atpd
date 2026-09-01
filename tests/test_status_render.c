#include "status.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static char *render_with_pid(pid_t pid) {
    status_snapshot_t snapshot = {0};
    snapshot.emoji_enabled = true;
    snapshot.daemon_running = true;
    snapshot.api_port = 9080;
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
    assert(fclose(stream) == 0);
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

    free(first);
    free(second);
    return 0;
}
