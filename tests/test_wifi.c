#include "wifi.h"
#include "atpd_context.h"
#include "utils.h"

#include <assert.h>
#include <string.h>
#include <time.h>

int main(void) {
    char ssid[64];

    assert(wifi_parse_ssid("Wifi is connected to \"Pixel AP\"\n", ssid,
                           sizeof(ssid)) == 0);
    assert(strcmp(ssid, "Pixel AP") == 0);
    assert(wifi_parse_ssid("WifiInfo: SSID: Home, BSSID: aa:bb\n", ssid,
                           sizeof(ssid)) == 0);
    assert(strcmp(ssid, "Home") == 0);
    assert(wifi_parse_ssid("WifiInfo: SSID: <unknown ssid>, BSSID: null\n", ssid,
                           sizeof(ssid)) != 0);
    assert(strcmp(atpd_clash_target_mode(VPN_STATE_READY,
                  DIRECT_WIFI_DISCONNECTED, "Rule"), "Google VPN") == 0);
    assert(strcmp(atpd_clash_target_mode(VPN_STATE_TEARDOWN,
                  DIRECT_WIFI_DISCONNECTED, "Rule"), "Google VPN") == 0);
    assert(strcmp(atpd_clash_target_mode(VPN_STATE_IDLE,
                  DIRECT_WIFI_ACTIVE, "Global"), "Direct") == 0);
    assert(strcmp(atpd_clash_target_mode(VPN_STATE_IDLE,
                  DIRECT_WIFI_DISCONNECTED, "Global"), "Global") == 0);

    char *timeout_argv[] = {"/bin/sh", "-c", "sleep 2", NULL};
    struct timespec start;
    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    assert(exec_cmd_argv(timeout_argv[0], timeout_argv, NULL, 0, 1) == 124);
    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = (double)(end.tv_sec - start.tv_sec) +
                     (double)(end.tv_nsec - start.tv_nsec) / 1000000000.0;
    assert(elapsed < 1.8);
    return 0;
}
