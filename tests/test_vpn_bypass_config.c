#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <arpa/inet.h>

static atp_result_t load_test_config(const char *content, atp_config_t *cfg) {
    char template[] = "/tmp/atpd_test_cfg_XXXXXX";
    int fd = mkstemp(template);
    assert(fd >= 0);
    size_t len = strlen(content);
    ssize_t written = write(fd, content, len);
    assert(written == (ssize_t)len);
    close(fd);

    atp_result_t res = config_load(template, cfg);
    unlink(template);
    return res;
}

int main(void) {
    printf("=== Test VPN Bypass Config Parsing ===\n");

    atp_config_t cfg;

    /* 1. Default configuration */
    atp_result_t res = load_test_config("# empty config\n", &cfg);
    assert(res == ATP_OK);
    assert(cfg.interface.vpn_bypass_route_count == 0);
    assert(cfg.interface.vpn_bypass_pref == 12400);

    /* 2. Valid mixed list */
    const char *valid_mixed =
        "VPN_BYPASS_ROUTES=\"100.64.0.0/10, fd7a:115c:a1e0::/48 192.168.88.0/24\"\n";
    res = load_test_config(valid_mixed, &cfg);
    assert(res == ATP_OK);
    assert(cfg.interface.vpn_bypass_route_count == 3);

    /* Route 0: 100.64.0.0/10 (IPv4) */
    assert(cfg.interface.vpn_bypass_routes[0].family == AF_INET);
    assert(cfg.interface.vpn_bypass_routes[0].prefix_len == 10);
    struct in_addr expected_v4_1;
    assert(inet_pton(AF_INET, "100.64.0.0", &expected_v4_1) == 1);
    assert(memcmp(cfg.interface.vpn_bypass_routes[0].addr, &expected_v4_1, 4) == 0);

    /* Route 1: fd7a:115c:a1e0::/48 (IPv6) */
    assert(cfg.interface.vpn_bypass_routes[1].family == AF_INET6);
    assert(cfg.interface.vpn_bypass_routes[1].prefix_len == 48);
    struct in6_addr expected_v6_1;
    assert(inet_pton(AF_INET6, "fd7a:115c:a1e0::", &expected_v6_1) == 1);
    assert(memcmp(cfg.interface.vpn_bypass_routes[1].addr, &expected_v6_1, 16) == 0);

    /* Route 2: 192.168.88.0/24 (IPv4) */
    assert(cfg.interface.vpn_bypass_routes[2].family == AF_INET);
    assert(cfg.interface.vpn_bypass_routes[2].prefix_len == 24);
    struct in_addr expected_v4_2;
    assert(inet_pton(AF_INET, "192.168.88.0", &expected_v4_2) == 1);
    assert(memcmp(cfg.interface.vpn_bypass_routes[2].addr, &expected_v4_2, 4) == 0);

    /* 3. Host bits masked off: 192.168.88.7/24 -> 192.168.88.0 */
    res = load_test_config("VPN_BYPASS_ROUTES=\"192.168.88.7/24\"\n", &cfg);
    assert(res == ATP_OK);
    assert(cfg.interface.vpn_bypass_route_count == 1);
    assert(cfg.interface.vpn_bypass_routes[0].family == AF_INET);
    assert(cfg.interface.vpn_bypass_routes[0].prefix_len == 24);
    assert(memcmp(cfg.interface.vpn_bypass_routes[0].addr, &expected_v4_2, 4) == 0);

    /* 4. Empty value -> 0 routes */
    res = load_test_config("VPN_BYPASS_ROUTES=\"\"\n", &cfg);
    assert(res == ATP_OK);
    assert(cfg.interface.vpn_bypass_route_count == 0);

    /* 5. Custom pref parsed: VPN_BYPASS_PREF=12000 */
    res = load_test_config("VPN_BYPASS_PREF=12000\n", &cfg);
    assert(res == ATP_OK);
    assert(cfg.interface.vpn_bypass_pref == 12000);

    /* 6. Invalid cases rejected with ATP_ERR_CONFIG */
    assert(load_test_config("VPN_BYPASS_ROUTES=\"100.64.0.0\"\n", &cfg) == ATP_ERR_CONFIG);
    assert(load_test_config("VPN_BYPASS_ROUTES=\"100.64.0.0/33\"\n", &cfg) == ATP_ERR_CONFIG);
    assert(load_test_config("VPN_BYPASS_ROUTES=\"0.0.0.0/0\"\n", &cfg) == ATP_ERR_CONFIG);
    assert(load_test_config("VPN_BYPASS_ROUTES=\"fd7a::/129\"\n", &cfg) == ATP_ERR_CONFIG);
    assert(load_test_config("VPN_BYPASS_ROUTES=\"nonsense/8\"\n", &cfg) == ATP_ERR_CONFIG);

    /* Exactly 16 entries succeeds */
    const char *sixteen_entries =
        "VPN_BYPASS_ROUTES=\"10.0.0.0/24 10.0.1.0/24 10.0.2.0/24 10.0.3.0/24 "
        "10.0.4.0/24 10.0.5.0/24 10.0.6.0/24 10.0.7.0/24 "
        "10.0.8.0/24 10.0.9.0/24 10.0.10.0/24 10.0.11.0/24 "
        "10.0.12.0/24 10.0.13.0/24 10.0.14.0/24 10.0.15.0/24\"\n";
    res = load_test_config(sixteen_entries, &cfg);
    assert(res == ATP_OK);
    assert(cfg.interface.vpn_bypass_route_count == 16);

    /* 17 entries rejected */
    const char *seventeen_entries =
        "VPN_BYPASS_ROUTES=\"10.0.0.0/24 10.0.1.0/24 10.0.2.0/24 10.0.3.0/24 "
        "10.0.4.0/24 10.0.5.0/24 10.0.6.0/24 10.0.7.0/24 "
        "10.0.8.0/24 10.0.9.0/24 10.0.10.0/24 10.0.11.0/24 "
        "10.0.12.0/24 10.0.13.0/24 10.0.14.0/24 10.0.15.0/24 10.0.16.0/24\"\n";
    assert(load_test_config(seventeen_entries, &cfg) == ATP_ERR_CONFIG);

    /* VPN_BYPASS_PREF bounds rejected */
    assert(load_test_config("VPN_BYPASS_PREF=0\n", &cfg) == ATP_ERR_CONFIG);
    assert(load_test_config("VPN_BYPASS_PREF=40000\n", &cfg) == ATP_ERR_CONFIG);

    /* 7. Classify-reload check */
    atp_config_t current, candidate;
    config_set_defaults(&current);
    config_set_defaults(&candidate);
    char changed[512] = {0};

    config_reload_changes_t changes =
        config_classify_reload(&current, &candidate, changed, sizeof(changed));
    assert(changes == CONFIG_RELOAD_CHANGE_NONE);

    candidate.interface.vpn_bypass_route_count = 1;
    candidate.interface.vpn_bypass_routes[0].family = AF_INET;
    candidate.interface.vpn_bypass_routes[0].prefix_len = 24;
    changes = config_classify_reload(&current, &candidate, changed, sizeof(changed));
    assert((changes & CONFIG_RELOAD_CHANGE_REQUIRES_RESTART) != 0);
    assert(strstr(changed, "VPN_BYPASS_ROUTES") != NULL);

    candidate = current;
    candidate.interface.vpn_bypass_pref = 12000;
    changed[0] = '\0';
    changes = config_classify_reload(&current, &candidate, changed, sizeof(changed));
    assert((changes & CONFIG_RELOAD_CHANGE_REQUIRES_RESTART) != 0);
    assert(strstr(changed, "VPN_BYPASS_PREF") != NULL);

    printf("=== Test VPN Bypass Config Parsing PASSED ===\n");
    return 0;
}
