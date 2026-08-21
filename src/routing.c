#include "routing.h"
#include "atp.h"
#include "atpd_context.h"
#include "logger.h"
#include "netlink.h"
#include "utils.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define IP_CMD "/system/bin/ip"
#define ATP_PREF_HOTSPOT 100
#define MAX_HOTSPOTS 16
#define MAX_OWNED_RULES 64

static pthread_mutex_t g_routing_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    char iface[IFNAMSIZ];
    uint32_t table;
} hotspot_rule_t;

static int valid_iface(const char *iface) {
    if (!iface || !iface[0] || strlen(iface) >= IFNAMSIZ) return 0;
    for (const char *p = iface; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' ||
              *p == '+' || *p == '.')) return 0;
    }
    return 1;
}

static int hotspot_name_matches(const char *pattern, const char *iface) {
    size_t len = strlen(pattern);
    if (len > 0 && (pattern[len - 1] == '+' || pattern[len - 1] == '*')) {
        return strncmp(pattern, iface, len - 1) == 0;
    }
    return strcmp(pattern, iface) == 0;
}

static int auto_hotspot_name(const char *iface) {
    size_t len = strlen(iface);
    return strncmp(iface, "ap_br_", 6) == 0 || strcmp(iface, "ap0") == 0 ||
           strncmp(iface, "softap", 6) == 0 || strncmp(iface, "rndis", 5) == 0 ||
           (strncmp(iface, "wlan", 4) == 0 && len >= 3 &&
            strcmp(iface + len - 3, "_ap") == 0);
}

static int managed_hotspot_name(const atp_config_t *cfg, const char *iface) {
    return cfg->interface.hotspot_iface_explicit
        ? hotspot_name_matches(cfg->interface.hotspot_iface, iface)
        : auto_hotspot_name(iface);
}

static int collect_hotspots(const atp_config_t *cfg,
                            char names[][IFNAMSIZ], int max) {
    struct ifaddrs *ifaddr;
    if (getifaddrs(&ifaddr) != 0) return 0;

    int count = 0;
    for (struct ifaddrs *ifa = ifaddr; ifa && count < max; ifa = ifa->ifa_next) {
        if (!ifa->ifa_name || !(ifa->ifa_flags & IFF_UP) ||
            !managed_hotspot_name(cfg, ifa->ifa_name)) continue;

        int duplicate = 0;
        for (int i = 0; i < count; i++) {
            if (strcmp(names[i], ifa->ifa_name) == 0) duplicate = 1;
        }
        if (!duplicate) snprintf(names[count++], IFNAMSIZ, "%s", ifa->ifa_name);
    }
    freeifaddrs(ifaddr);

    if (count == 0 && !cfg->interface.hotspot_iface_explicit &&
        cfg->interface.hotspot_iface[0] &&
        if_nametoindex(cfg->interface.hotspot_iface) != 0) {
        snprintf(names[count++], IFNAMSIZ, "%s", cfg->interface.hotspot_iface);
    }
    return count;
}

static int read_managed_rules(const atp_config_t *cfg, int family,
                              hotspot_rule_t *rules, int max) {
    char command[128];
    int n = snprintf(command, sizeof(command), "%s %srule show 2>/dev/null",
                     IP_CMD, family == AF_INET6 ? "-6 " : "");
    if (n < 0 || n >= (int)sizeof(command)) return 0;

    FILE *fp = popen(command, "r");
    if (!fp) return 0;

    int count = 0;
    char line[256];
    while (count < max && fgets(line, sizeof(line), fp)) {
        unsigned pref = 0;
        unsigned table = 0;
        char iface[IFNAMSIZ] = {0};
        if (sscanf(line, "%u: from all iif %15s lookup %u",
                   &pref, iface, &table) != 3 || pref != ATP_PREF_HOTSPOT ||
            !valid_iface(iface) || !managed_hotspot_name(cfg, iface)) continue;
        snprintf(rules[count].iface, sizeof(rules[count].iface), "%s", iface);
        rules[count].table = table;
        count++;
    }
    pclose(fp);
    return count;
}

static int run_rule(const atp_config_t *cfg, int family, const char *action,
                    const char *iface, uint32_t table) {
    if (!valid_iface(iface)) return ATP_ERR_INVAL;
    if (cfg->core.dry_run) return ATP_OK;

    char command[MAX_CMD_LEN];
    int n = snprintf(command, sizeof(command),
                     "%s %srule %s from all iif %s lookup %u pref %d 2>/dev/null",
                     IP_CMD, family == AF_INET6 ? "-6 " : "", action,
                     iface, table, ATP_PREF_HOTSPOT);
    if (n < 0 || n >= (int)sizeof(command)) return ATP_ERR_INVAL;
    return exec_cmd_simple(command, CMD_TIMEOUT_SEC);
}

static int rule_exists(const atp_config_t *cfg, int family,
                       const char *iface, uint32_t table) {
    hotspot_rule_t rules[MAX_OWNED_RULES];
    int count = read_managed_rules(cfg, family, rules, MAX_OWNED_RULES);
    for (int i = 0; i < count; i++) {
        if (rules[i].table == table && strcmp(rules[i].iface, iface) == 0) return 1;
    }
    return 0;
}

static int remove_managed_rules(const atp_config_t *cfg, int family) {
    hotspot_rule_t rules[MAX_OWNED_RULES];
    int count = read_managed_rules(cfg, family, rules, MAX_OWNED_RULES);
    int removed = 0;
    for (int i = 0; i < count; i++) {
        if (run_rule(cfg, family, "del", rules[i].iface, rules[i].table) == ATP_OK) {
            removed++;
        }
    }
    return removed;
}

static void update_snapshot(uint32_t table, int ipv4_default, int ipv6_default,
                            char names[][IFNAMSIZ], int count,
                            int ipv4_active, int ipv6_active) {
    g_atpd_ctx.vpn_route_table = table;
    g_atpd_ctx.vpn_ipv4_default = ipv4_default != 0;
    g_atpd_ctx.vpn_ipv6_default = ipv6_default != 0;
    g_atpd_ctx.hotspot_count = count > 0 ? (unsigned)count : 0;
    g_atpd_ctx.hotspot_ipv4_active = ipv4_active > 0 ? (unsigned)ipv4_active : 0;
    g_atpd_ctx.hotspot_ipv6_active = ipv6_active > 0 ? (unsigned)ipv6_active : 0;
    g_atpd_ctx.hotspot_ifaces[0] = '\0';

    size_t used = 0;
    for (int i = 0; i < count; i++) {
        int written = snprintf(g_atpd_ctx.hotspot_ifaces + used,
                               sizeof(g_atpd_ctx.hotspot_ifaces) - used,
                               "%s%s", i ? ", " : "", names[i]);
        if (written < 0 || (size_t)written >= sizeof(g_atpd_ctx.hotspot_ifaces) - used) break;
        used += (size_t)written;
    }
    g_atpd_ctx.policy_last_reconcile = (uint64_t)time(NULL);
}

int routing_add_vpn_policy(atp_config_t *cfg, const char *vpn_iface) {
    if (!cfg || !valid_iface(vpn_iface)) return ATP_ERR_INVAL;

    uint32_t table = 0;
    char hotspots[MAX_HOTSPOTS][IFNAMSIZ] = {{0}};
    if (netlink_get_vpn_table(vpn_iface, &table) != 0) {
        LOG_WARN("VPN policy table not ready for %s", vpn_iface);
        update_snapshot(0, 0, 0, hotspots, 0, 0, 0);
        return ATP_ERR_NOENT;
    }

    int ipv4_default = netlink_table_has_default_route(table, AF_INET);
    int ipv6_default = netlink_table_has_default_route(table, AF_INET6);
    if (!ipv4_default) {
        LOG_WARN("VPN table %u has no IPv4 default route", table);
        update_snapshot(table, 0, ipv6_default, hotspots, 0, 0, 0);
        return ATP_ERR_NOENT;
    }

    int count = collect_hotspots(cfg, hotspots, MAX_HOTSPOTS);
    if (count == 0) {
        update_snapshot(table, ipv4_default, ipv6_default, hotspots, 0, 0, 0);
        return ATP_OK;
    }

    pthread_mutex_lock(&g_routing_mutex);
    int active4 = 0;
    int active6 = 0;
    for (int i = 0; i < count; i++) {
        if (rule_exists(cfg, AF_INET, hotspots[i], table)) active4++;
        if (ipv6_default && rule_exists(cfg, AF_INET6, hotspots[i], table)) active6++;
    }

    int healthy = strcmp(cfg->interface.current_vpn_iface, vpn_iface) == 0 &&
                  active4 == count && (!ipv6_default || active6 == count);
    if (!healthy) {
        remove_managed_rules(cfg, AF_INET);
        remove_managed_rules(cfg, AF_INET6);
        active4 = 0;
        active6 = 0;
        for (int i = 0; i < count; i++) {
            if (run_rule(cfg, AF_INET, "add", hotspots[i], table) == ATP_OK) active4++;
            if (ipv6_default &&
                run_rule(cfg, AF_INET6, "add", hotspots[i], table) == ATP_OK) active6++;
        }
    }

    update_snapshot(table, ipv4_default, ipv6_default,
                    hotspots, count, active4, active6);
    if (active4 > 0) {
        snprintf(cfg->interface.current_vpn_iface,
                 sizeof(cfg->interface.current_vpn_iface), "%s", vpn_iface);
    }
    pthread_mutex_unlock(&g_routing_mutex);

    if (active4 == 0) return ATP_ERR_GENERAL;
    if (!healthy) {
        LOG_INFO("Hotspot VPN policy synchronized: IPv4=%d IPv6=%d interface(s) -> table %u",
                 active4, active6, table);
    }
    return ATP_OK;
}

int routing_remove_vpn_policy(atp_config_t *cfg, const char *vpn_iface) {
    if (!cfg) return ATP_ERR_INVAL;
    pthread_mutex_lock(&g_routing_mutex);
    remove_managed_rules(cfg, AF_INET);
    remove_managed_rules(cfg, AF_INET6);
    cfg->interface.current_vpn_iface[0] = '\0';
    char no_hotspots[1][IFNAMSIZ] = {{0}};
    update_snapshot(0, 0, 0, no_hotspots, 0, 0, 0);
    pthread_mutex_unlock(&g_routing_mutex);
    if (vpn_iface && vpn_iface[0]) LOG_INFO("Hotspot VPN policy removed for %s", vpn_iface);
    return ATP_OK;
}
