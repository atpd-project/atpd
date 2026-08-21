#include "wifi.h"
#include "utils.h"

#include <stdio.h>
#include <string.h>

static int copy_ssid(const char *start, char *ssid, size_t size) {
    while (*start == ' ' || *start == '\t') start++;

    const char *end;
    if (*start == '"') {
        start++;
        end = strchr(start, '"');
    } else {
        end = strpbrk(start, ",\r\n");
    }
    if (!end) end = start + strlen(start);
    while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;

    size_t len = (size_t)(end - start);
    if (!len || len >= size ||
        (len == 14 && strncmp(start, "<unknown ssid>", len) == 0) ||
        (len == 4 && strncmp(start, "null", len) == 0)) {
        return -1;
    }
    memcpy(ssid, start, len);
    ssid[len] = '\0';
    return 0;
}

int wifi_parse_ssid(const char *status, char *ssid, size_t size) {
    if (!status || !ssid || size < 2) return -1;
    ssid[0] = '\0';

    const char *connected = strstr(status, "connected to \"");
    if (connected && copy_ssid(connected + strlen("connected to "), ssid, size) == 0) {
        return 0;
    }

    const char *line = status;
    while ((line = strstr(line, "SSID:"))) {
        if (copy_ssid(line + strlen("SSID:"), ssid, size) == 0) return 0;
        line += strlen("SSID:");
    }
    return -1;
}

int wifi_get_ssid(char *ssid, size_t size) {
    char output[4096];
    char *const argv[] = { "cmd", "wifi", "status", NULL };
    if (exec_cmd_argv("/system/bin/cmd", argv, output, sizeof(output), 2) != 0) {
        if (ssid && size) ssid[0] = '\0';
        return -1;
    }
    return wifi_parse_ssid(output, ssid, size) == 0 ? 0 : 1;
}
