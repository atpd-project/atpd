#include "routing.h"
#include "logger.h"
#include "utils.h"
#include "config.h"
#include "atp.h"
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <sys/wait.h>

#define IP_CMD "/system/bin/ip"
#define ATP_PREF_VPN_LOCK 20000
#define ATP_PREF_HOTSPOT 100

static pthread_mutex_t g_routing_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_original_ipv4_forward = -1;
static int g_original_ipv6_forward = -1;

static int validate_iface_arg(const char *iface) {
    if (!iface || !*iface) return ATP_ERR_INVAL;

    if (strlen(iface) >= IFNAMSIZ) {
        LOG_ERROR("Interface name exceeds IFNAMSIZ: %s", iface);
        return ATP_ERR_INVAL;
    }

    for (const char *p = iface; *p; p++) {
        if (!(*p >= 'a' && *p <= 'z') &&
            !(*p >= 'A' && *p <= 'Z') &&
            !(*p >= '0' && *p <= '9') &&
            *p != '_' && *p != '-' && *p != '+' && *p != '.') {
            LOG_ERROR("Invalid character in interface name: %s", iface);
            return ATP_ERR_INVAL;
        }
    }

    return ATP_OK;
}

static int exec_ip(atp_config_t *cfg, const char *cmd, const char *arg) {
    if (cfg->core.dry_run) {
        LOG_DEBUG("[DRY_RUN] ip %s %s", cmd, arg ? arg : "");
        return 0;
    }

    char command[MAX_CMD_LEN];
    int n;

    if (arg) {
        n = snprintf(command, sizeof(command), "%s %s %s 2>/dev/null", IP_CMD, cmd, arg);
    } else {
        n = snprintf(command, sizeof(command), "%s %s 2>/dev/null", IP_CMD, cmd);
    }

    if (n < 0 || n >= (int)sizeof(command)) {
        LOG_ERROR("Command buffer truncated");
        return ATP_ERR_INVAL;
    }

    return exec_cmd_simple(command, CMD_TIMEOUT_SEC);
}

static int exec_ip6(atp_config_t *cfg, const char *cmd, const char *arg) {
    if (cfg->core.dry_run) {
        LOG_DEBUG("[DRY_RUN] ip -6 %s %s", cmd, arg ? arg : "");
        return 0;
    }

    char command[MAX_CMD_LEN];
    int n;

    if (arg) {
        n = snprintf(command, sizeof(command), "%s -6 %s %s 2>/dev/null", IP_CMD, cmd, arg);
    } else {
        n = snprintf(command, sizeof(command), "%s -6 %s 2>/dev/null", IP_CMD, cmd);
    }

    if (n < 0 || n >= (int)sizeof(command)) {
        LOG_ERROR("Command buffer truncated");
        return ATP_ERR_INVAL;
    }

    return exec_cmd_simple(command, CMD_TIMEOUT_SEC);
}

static int exec_cmd_status(const char *cmd, int timeout_sec) {
    if (!cmd) return -1;

    char timeout_cmd[MAX_CMD_LEN];
    int n = snprintf(timeout_cmd, sizeof(timeout_cmd), "timeout %d %s", timeout_sec, cmd);
    if (n < 0 || n >= (int)sizeof(timeout_cmd)) {
        LOG_ERROR("Command buffer truncated");
        return -1;
    }

    FILE *fp = popen(timeout_cmd, "r");
    if (!fp) {
        LOG_ERROR("popen failed: %s", strerror(errno));
        return -1;
    }

    int status = pclose(fp);
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    return exit_code;
}

static int write_sysctl(const char *path, const char *value) {
    if (!path || !value) return ATP_ERR_INVAL;

    FILE *fp = fopen(path, "w");
    if (!fp) {
        LOG_ERROR("Failed to open %s: %s", path, strerror(errno));
        return ATP_ERR_IO;
    }

    if (fprintf(fp, "%s\n", value) < 0) {
        fclose(fp);
        return ATP_ERR_IO;
    }

    if (fflush(fp) != 0) {
        fclose(fp);
        return ATP_ERR_IO;
    }

    if (fsync(fileno(fp)) != 0) {
        fclose(fp);
        return ATP_ERR_IO;
    }

    if (fclose(fp) != 0) {
        return ATP_ERR_IO;
    }

    return ATP_OK;
}

static int read_sysctl_int(const char *path, int *value) {
    if (!path || !value) return ATP_ERR_INVAL;

    FILE *fp = fopen(path, "r");
    if (!fp) {
        return ATP_ERR_IO;
    }

    if (fscanf(fp, "%d", value) != 1) {
        fclose(fp);
        return ATP_ERR_IO;
    }

    fclose(fp);
    return ATP_OK;
}

int routing_rule_add(atp_config_t *cfg, int family, const char *rule) {
    if (!rule) return ATP_ERR_INVAL;

    if (family == 4) {
        return exec_ip(cfg, "rule add", rule);
    } else {
        return exec_ip6(cfg, "rule add", rule);
    }
}

int routing_rule_del(atp_config_t *cfg, int family, const char *rule) {
    if (!rule) return ATP_ERR_INVAL;

    if (family == 4) {
        return exec_ip(cfg, "rule del", rule);
    } else {
        return exec_ip6(cfg, "rule del", rule);
    }
}

int routing_rule_del_by_pref(atp_config_t *cfg, int family, int pref) {
    char rule_buf[64];
    int n = snprintf(rule_buf, sizeof(rule_buf), "pref %d", pref);
    if (n < 0 || n >= (int)sizeof(rule_buf)) {
        return ATP_ERR_INVAL;
    }
    return routing_rule_del(cfg, family, rule_buf);
}

int routing_rule_del_all_by_pref(atp_config_t *cfg, int family, int pref) {
    int count = 0;
    char check_buf[256];
    int n;

    if (cfg->core.dry_run) {
        LOG_DEBUG("[DRY_RUN] Would delete all rules with pref %d", pref);
        return 0;
    }

    if (family == 4) {
        n = snprintf(check_buf, sizeof(check_buf), "%s rule show | grep 'pref %d'", IP_CMD, pref);
    } else {
        n = snprintf(check_buf, sizeof(check_buf), "%s -6 rule show | grep 'pref %d'", IP_CMD, pref);
    }

    if (n < 0 || n >= (int)sizeof(check_buf)) {
        LOG_ERROR("Buffer truncated for rule check");
        return ATP_ERR_INVAL;
    }

    while (count < 100) {
        int ret = exec_cmd_status(check_buf, 5);
        if (ret != 0) break;

        int del_ret = routing_rule_del_by_pref(cfg, family, pref);
        if (del_ret != 0) {
            LOG_WARN("Failed to delete routing rule pref=%d", pref);
            break;
        }

        count++;
    }

    if (count >= 100) {
        LOG_WARN("Delete loop limit reached for pref=%d", pref);
    }

    return count;
}

int routing_route_add(atp_config_t *cfg, int family, const char *route) {
    if (!route) return ATP_ERR_INVAL;

    if (family == 4) {
        return exec_ip(cfg, "route add", route);
    } else {
        return exec_ip6(cfg, "route add", route);
    }
}

int routing_route_del(atp_config_t *cfg, int family, const char *route) {
    if (!route) return ATP_ERR_INVAL;

    if (family == 4) {
        return exec_ip(cfg, "route del", route);
    } else {
        return exec_ip6(cfg, "route del", route);
    }
}

int routing_route_flush_table(atp_config_t *cfg, int family, int table_id) {
    char flush_buf[64];
    int n = snprintf(flush_buf, sizeof(flush_buf), "table %d", table_id);
    if (n < 0 || n >= (int)sizeof(flush_buf)) {
        return ATP_ERR_INVAL;
    }

    if (family == 4) {
        return exec_ip(cfg, "route flush", flush_buf);
    } else {
        return exec_ip6(cfg, "route flush", flush_buf);
    }
}

static int routing_rule_exists(atp_config_t *cfg, int family, int mark, int table_id) {
    char cmd[MAX_CMD_LEN];
    int n;

    if (cfg->core.dry_run) {
        LOG_DEBUG("[DRY_RUN] Check routing rule exists: fwmark 0x%x table %d", mark, table_id);
        return 0;
    }

    if (family == 4) {
        n = snprintf(cmd, sizeof(cmd),
                     "%s rule show | grep -q 'fwmark 0x%x.*lookup %d '", IP_CMD, mark, table_id);
    } else {
        n = snprintf(cmd, sizeof(cmd),
                     "%s -6 rule show | grep -q 'fwmark 0x%x.*lookup %d '", IP_CMD, mark, table_id);
    }

    if (n < 0 || n >= (int)sizeof(cmd)) {
        LOG_ERROR("Buffer truncated for rule exists check");
        return 0;
    }

    return exec_cmd_status(cmd, 5) == 0;
}

/*
 * Must be called while g_routing_mutex is held.
 */
static void save_original_forward_state(void) {
    if (g_original_ipv4_forward == -1) {
        int val;
        if (read_sysctl_int("/proc/sys/net/ipv4/ip_forward", &val) == ATP_OK) {
            g_original_ipv4_forward = val;
            LOG_DEBUG("Saved original IPv4 forward: %d", val);
        } else {
            g_original_ipv4_forward = 0;
        }
    }

    if (g_original_ipv6_forward == -1) {
        int val;
        if (read_sysctl_int("/proc/sys/net/ipv6/conf/all/forwarding", &val) == ATP_OK) {
            g_original_ipv6_forward = val;
            LOG_DEBUG("Saved original IPv6 forward: %d", val);
        } else {
            g_original_ipv6_forward = 0;
        }
    }
}

int routing_setup_ipv4(atp_config_t *cfg) {
    int ret;

    pthread_mutex_lock(&g_routing_mutex);

    LOG_INFO("Setting up IPv4 policy routing (table=%d, mark=%d)",
             cfg->network.table_id, cfg->network.mark_value);

    save_original_forward_state();

    routing_rule_del_all_by_pref(cfg, 4, cfg->network.table_id);

    if (!routing_rule_exists(cfg, 4, cfg->network.mark_value, cfg->network.table_id)) {
        char rule_buf[128];
        int n = snprintf(rule_buf, sizeof(rule_buf), "fwmark 0x%x table %d pref %d",
                         cfg->network.mark_value, cfg->network.table_id, cfg->network.table_id);
        if (n < 0 || n >= (int)sizeof(rule_buf)) {
            LOG_ERROR("Rule buffer truncated");
            pthread_mutex_unlock(&g_routing_mutex);
            return ATP_ERR_INVAL;
        }
        ret = routing_rule_add(cfg, 4, rule_buf);
        if (ret != 0) {
            LOG_ERROR("Failed to add IPv4 routing rule");
            pthread_mutex_unlock(&g_routing_mutex);
            return ret;
        }
        LOG_DEBUG("Added IPv4 routing rule");
    } else {
        LOG_DEBUG("IPv4 routing rule already exists, skipping");
    }

    char route_buf[128];
    int n = snprintf(route_buf, sizeof(route_buf), "local 0.0.0.0/0 dev lo table %d",
                     cfg->network.table_id);
    if (n < 0 || n >= (int)sizeof(route_buf)) {
        LOG_ERROR("Route buffer truncated");
        pthread_mutex_unlock(&g_routing_mutex);
        return ATP_ERR_INVAL;
    }

    ret = routing_route_add(cfg, 4, route_buf);
    if (ret != 0) {
        LOG_ERROR("Failed to add IPv4 local route");
        pthread_mutex_unlock(&g_routing_mutex);
        return ret;
    }

    ret = routing_ip_forward_enable(cfg, 1);
    if (ret != 0) {
        LOG_WARN("Failed to enable IPv4 forwarding");
    }

    LOG_INFO("IPv4 routing setup complete");
    pthread_mutex_unlock(&g_routing_mutex);
    return 0;
}

int routing_setup_ipv6(atp_config_t *cfg) {
    int ret;

    if (!cfg->network.proxy_ipv6) {
        LOG_DEBUG("IPv6 proxy disabled, skipping routing");
        return 0;
    }

    pthread_mutex_lock(&g_routing_mutex);

    LOG_INFO("Setting up IPv6 policy routing (table=%d, mark=%d)",
             cfg->network.table_id, cfg->network.mark_value6);

    save_original_forward_state();

    routing_rule_del_all_by_pref(cfg, 6, cfg->network.table_id);

    if (!routing_rule_exists(cfg, 6, cfg->network.mark_value6, cfg->network.table_id)) {
        char rule_buf[128];
        int n = snprintf(rule_buf, sizeof(rule_buf), "fwmark 0x%x table %d pref %d",
                         cfg->network.mark_value6, cfg->network.table_id, cfg->network.table_id);
        if (n < 0 || n >= (int)sizeof(rule_buf)) {
            LOG_ERROR("Rule buffer truncated");
            pthread_mutex_unlock(&g_routing_mutex);
            return ATP_ERR_INVAL;
        }
        ret = routing_rule_add(cfg, 6, rule_buf);
        if (ret != 0) {
            LOG_ERROR("Failed to add IPv6 routing rule");
            pthread_mutex_unlock(&g_routing_mutex);
            return ret;
        }
        LOG_DEBUG("Added IPv6 routing rule");
    } else {
        LOG_DEBUG("IPv6 routing rule already exists, skipping");
    }

    char route_buf[128];
    int n = snprintf(route_buf, sizeof(route_buf), "local ::/0 dev lo table %d",
                     cfg->network.table_id);
    if (n < 0 || n >= (int)sizeof(route_buf)) {
        LOG_ERROR("Route buffer truncated");
        pthread_mutex_unlock(&g_routing_mutex);
        return ATP_ERR_INVAL;
    }

    ret = routing_route_add(cfg, 6, route_buf);
    if (ret != 0) {
        LOG_ERROR("Failed to add IPv6 local route");
        pthread_mutex_unlock(&g_routing_mutex);
        return ret;
    }

    ret = routing_ipv6_forward_enable(cfg, 1);
    if (ret != 0) {
        LOG_WARN("Failed to enable IPv6 forwarding");
    }

    LOG_INFO("IPv6 routing setup complete");
    pthread_mutex_unlock(&g_routing_mutex);
    return 0;
}

int routing_cleanup_ipv4(atp_config_t *cfg) {
    int ret;

    pthread_mutex_lock(&g_routing_mutex);

    LOG_INFO("Cleaning up IPv4 policy routing");

    routing_rule_del_all_by_pref(cfg, 4, cfg->network.table_id);

    char route_buf[128];
    int n = snprintf(route_buf, sizeof(route_buf), "local 0.0.0.0/0 dev lo table %d",
                     cfg->network.table_id);
    if (n < 0 || n >= (int)sizeof(route_buf)) {
        LOG_ERROR("Route buffer truncated");
        pthread_mutex_unlock(&g_routing_mutex);
        return ATP_ERR_INVAL;
    }

    routing_route_del(cfg, 4, route_buf);
    routing_route_flush_table(cfg, 4, cfg->network.table_id);

    if (g_original_ipv4_forward >= 0) {
        ret = routing_ip_forward_enable(cfg, g_original_ipv4_forward);
    } else {
        ret = routing_ip_forward_enable(cfg, 0);
    }
    if (ret != 0) {
        LOG_WARN("Failed to restore IPv4 forwarding");
    }

    LOG_INFO("IPv4 routing cleanup complete");
    pthread_mutex_unlock(&g_routing_mutex);
    return 0;
}

int routing_cleanup_ipv6(atp_config_t *cfg) {
    int ret;

    if (!cfg->network.proxy_ipv6) {
        LOG_DEBUG("IPv6 proxy disabled, skipping cleanup");
        return 0;
    }

    pthread_mutex_lock(&g_routing_mutex);

    LOG_INFO("Cleaning up IPv6 policy routing");

    routing_rule_del_all_by_pref(cfg, 6, cfg->network.table_id);

    char route_buf[128];
    int n = snprintf(route_buf, sizeof(route_buf), "local ::/0 dev lo table %d",
                     cfg->network.table_id);
    if (n < 0 || n >= (int)sizeof(route_buf)) {
        LOG_ERROR("Route buffer truncated");
        pthread_mutex_unlock(&g_routing_mutex);
        return ATP_ERR_INVAL;
    }

    routing_route_del(cfg, 6, route_buf);
    routing_route_flush_table(cfg, 6, cfg->network.table_id);

    if (g_original_ipv6_forward >= 0) {
        ret = routing_ipv6_forward_enable(cfg, g_original_ipv6_forward);
    } else {
        ret = routing_ipv6_forward_enable(cfg, 0);
    }
    if (ret != 0) {
        LOG_WARN("Failed to restore IPv6 forwarding");
    }

    LOG_INFO("IPv6 routing cleanup complete");
    pthread_mutex_unlock(&g_routing_mutex);
    return 0;
}

int routing_cleanup_all(atp_config_t *cfg) {
    routing_cleanup_ipv4(cfg);
    routing_cleanup_ipv6(cfg);
    return 0;
}

int routing_add_vpn_policy(atp_config_t *cfg, const char *vpn_iface) {
    int ret;

    if (validate_iface_arg(vpn_iface) != ATP_OK) {
        LOG_ERROR("Invalid VPN interface: %s", vpn_iface);
        return ATP_ERR_INVAL;
    }

    if (validate_iface_arg(cfg->interface.hotspot_iface) != ATP_OK) {
        LOG_ERROR("Invalid hotspot interface: %s", cfg->interface.hotspot_iface);
        return ATP_ERR_INVAL;
    }

    pthread_mutex_lock(&g_routing_mutex);

    LOG_INFO("Adding VPN policy for interface: %s", vpn_iface);

    char rule_buf[128];
    int n = snprintf(rule_buf, sizeof(rule_buf), "fwmark 0x20000 table %d pref %d",
                     cfg->network.table_id, ATP_PREF_VPN_LOCK);
    if (n < 0 || n >= (int)sizeof(rule_buf)) {
        LOG_ERROR("Rule buffer truncated");
        pthread_mutex_unlock(&g_routing_mutex);
        return ATP_ERR_INVAL;
    }

    ret = routing_rule_add(cfg, 4, rule_buf);
    if (ret != 0) {
        LOG_ERROR("Failed to add global fwmark lock");
        pthread_mutex_unlock(&g_routing_mutex);
        return ret;
    }
    LOG_DEBUG("Added global fwmark lock (pref %d)", ATP_PREF_VPN_LOCK);

    if (cfg->network.proxy_ipv6) {
        ret = routing_rule_add(cfg, 6, rule_buf);
        if (ret != 0) {
            LOG_ERROR("Failed to add IPv6 global fwmark lock");
            pthread_mutex_unlock(&g_routing_mutex);
            return ret;
        }
        LOG_DEBUG("Added IPv6 global fwmark lock");
    }

    n = snprintf(rule_buf, sizeof(rule_buf), "from all iif %s lookup %s pref %d",
                 cfg->interface.hotspot_iface, vpn_iface, ATP_PREF_HOTSPOT);
    if (n < 0 || n >= (int)sizeof(rule_buf)) {
        LOG_ERROR("Rule buffer truncated");
        pthread_mutex_unlock(&g_routing_mutex);
        return ATP_ERR_INVAL;
    }

    ret = routing_rule_add(cfg, 4, rule_buf);
    if (ret != 0) {
        LOG_ERROR("Failed to add hotspot policy");
        pthread_mutex_unlock(&g_routing_mutex);
        return ret;
    }
    LOG_DEBUG("Added hotspot policy (iif %s -> %s pref %d)",
              cfg->interface.hotspot_iface, vpn_iface, ATP_PREF_HOTSPOT);

    if (cfg->network.proxy_ipv6) {
        ret = routing_rule_add(cfg, 6, rule_buf);
        if (ret != 0) {
            LOG_ERROR("Failed to add IPv6 hotspot policy");
            pthread_mutex_unlock(&g_routing_mutex);
            return ret;
        }
        LOG_DEBUG("Added IPv6 hotspot policy");
    }

    snprintf(cfg->interface.current_vpn_iface, sizeof(cfg->interface.current_vpn_iface), "%s", vpn_iface);

    LOG_INFO("VPN policy added successfully");
    pthread_mutex_unlock(&g_routing_mutex);
    return 0;
}

int routing_remove_vpn_policy(atp_config_t *cfg, const char *vpn_iface) {
    const char *iface = vpn_iface;
    if (!iface || !iface[0]) {
        iface = cfg->interface.current_vpn_iface;
    }

    if (!iface || !iface[0]) {
        return 0;
    }

    pthread_mutex_lock(&g_routing_mutex);

    LOG_INFO("Removing VPN policy for interface: %s", iface);

    routing_rule_del_by_pref(cfg, 4, ATP_PREF_VPN_LOCK);
    LOG_DEBUG("Removed global fwmark lock");

    if (cfg->network.proxy_ipv6) {
        routing_rule_del_by_pref(cfg, 6, ATP_PREF_VPN_LOCK);
        LOG_DEBUG("Removed IPv6 global fwmark lock");
    }

    routing_rule_del_by_pref(cfg, 4, ATP_PREF_HOTSPOT);
    LOG_DEBUG("Removed hotspot policy");

    if (cfg->network.proxy_ipv6) {
        routing_rule_del_by_pref(cfg, 6, ATP_PREF_HOTSPOT);
        LOG_DEBUG("Removed IPv6 hotspot policy");
    }

    cfg->interface.current_vpn_iface[0] = '\0';

    LOG_INFO("VPN policy removed successfully");
    pthread_mutex_unlock(&g_routing_mutex);
    return 0;
}

int routing_add_mss_clamp(atp_config_t *cfg, const char *iface) {
    if (validate_iface_arg(iface) != ATP_OK) {
        LOG_ERROR("Invalid interface for MSS clamp: %s", iface);
        return ATP_ERR_INVAL;
    }

    LOG_INFO("Adding MSS clamp for interface: %s", iface);

    if (cfg->core.dry_run) {
        LOG_DEBUG("[DRY_RUN] iptables -t mangle -A FORWARD -o %s -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --clamp-mss-to-pmtu", iface);
        return 0;
    }

    char rule_buf[256];
    int n = snprintf(rule_buf, sizeof(rule_buf),
                     "-o %s -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --clamp-mss-to-pmtu",
                     iface);
    if (n < 0 || n >= (int)sizeof(rule_buf)) {
        LOG_ERROR("Rule buffer truncated");
        return ATP_ERR_INVAL;
    }

    char cmd[MAX_CMD_LEN];
    n = snprintf(cmd, sizeof(cmd),
                 "/system/bin/iptables -t mangle -C FORWARD %s 2>/dev/null", rule_buf);
    if (n < 0 || n >= (int)sizeof(cmd)) {
        LOG_ERROR("Command buffer truncated");
        return ATP_ERR_INVAL;
    }

    if (exec_cmd_simple(cmd, CMD_TIMEOUT_SEC) != 0) {
        n = snprintf(cmd, sizeof(cmd),
                     "/system/bin/iptables -t mangle -A FORWARD %s", rule_buf);
        if (n < 0 || n >= (int)sizeof(cmd)) {
            LOG_ERROR("Command buffer truncated");
            return ATP_ERR_INVAL;
        }
        exec_cmd_simple(cmd, CMD_TIMEOUT_SEC);
        LOG_DEBUG("MSS clamp rule added");
    } else {
        LOG_DEBUG("MSS clamp rule already exists");
    }

    return 0;
}

int routing_remove_mss_clamp(atp_config_t *cfg, const char *iface) {
    if (validate_iface_arg(iface) != ATP_OK) {
        LOG_ERROR("Invalid interface for MSS clamp removal: %s", iface);
        return ATP_ERR_INVAL;
    }

    LOG_INFO("Removing MSS clamp for interface: %s", iface);

    if (cfg->core.dry_run) {
        LOG_DEBUG("[DRY_RUN] iptables -t mangle -D FORWARD -o %s -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --clamp-mss-to-pmtu", iface);
        return 0;
    }

    char rule_buf[256];
    int n = snprintf(rule_buf, sizeof(rule_buf),
                     "-o %s -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --clamp-mss-to-pmtu",
                     iface);
    if (n < 0 || n >= (int)sizeof(rule_buf)) {
        LOG_ERROR("Rule buffer truncated");
        return ATP_ERR_INVAL;
    }

    char cmd[MAX_CMD_LEN];
    n = snprintf(cmd, sizeof(cmd),
                 "/system/bin/iptables -t mangle -D FORWARD %s 2>/dev/null", rule_buf);
    if (n < 0 || n >= (int)sizeof(cmd)) {
        LOG_ERROR("Command buffer truncated");
        return ATP_ERR_INVAL;
    }

    exec_cmd_simple(cmd, CMD_TIMEOUT_SEC);

    LOG_DEBUG("MSS clamp rule removed");
    return 0;
}

int routing_ip_forward_enable(atp_config_t *cfg, int enable) {
    char val[8];
    snprintf(val, sizeof(val), "%d", enable);

    if (cfg->core.dry_run) {
        LOG_DEBUG("[DRY_RUN] echo %d > /proc/sys/net/ipv4/ip_forward", enable);
        return 0;
    }

    return write_sysctl("/proc/sys/net/ipv4/ip_forward", val);
}

int routing_ipv6_forward_enable(atp_config_t *cfg, int enable) {
    char val[8];
    snprintf(val, sizeof(val), "%d", enable);

    if (cfg->core.dry_run) {
        LOG_DEBUG("[DRY_RUN] echo %d > /proc/sys/net/ipv6/conf/all/forwarding", enable);
        return 0;
    }

    return write_sysctl("/proc/sys/net/ipv6/conf/all/forwarding", val);
}

int routing_rp_filter_set(atp_config_t *cfg, int value) {
    char path[256];
    DIR *dir;
    struct dirent *entry;
    char val[8];
    int ret = ATP_OK;

    snprintf(val, sizeof(val), "%d", value);

    dir = opendir("/proc/sys/net/ipv4/conf");
    if (!dir) return ATP_ERR_IO;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        snprintf(path, sizeof(path), "/proc/sys/net/ipv4/conf/%s/rp_filter", entry->d_name);

        if (cfg->core.dry_run) {
            LOG_DEBUG("[DRY_RUN] echo %d > %s", value, path);
            continue;
        }

        if (write_sysctl(path, val) != ATP_OK) {
            LOG_WARN("Failed to set rp_filter for %s", entry->d_name);
            ret = ATP_ERR_IO;
        }
    }

    closedir(dir);
    LOG_DEBUG("rp_filter set to %d for all interfaces", value);
    return ret;
}

int routing_tcp_stack_tune(atp_config_t *cfg) {
    if (cfg->core.dry_run) {
        LOG_DEBUG("[DRY_RUN] TCP stack tuning skipped");
        return 0;
    }

    write_sysctl("/proc/sys/net/ipv4/tcp_fastopen", "3");
    write_sysctl("/proc/sys/net/core/rmem_max", "16777216");
    write_sysctl("/proc/sys/net/core/wmem_max", "16777216");
    write_sysctl("/proc/sys/net/ipv4/tcp_rmem", "4096 87380 16777216");
    write_sysctl("/proc/sys/net/ipv4/tcp_wmem", "4096 65536 16777216");

    if (exec_cmd_status("grep -q bbr /proc/sys/net/ipv4/tcp_allowed_congestion_control 2>/dev/null", 5) != 0) {
        write_sysctl("/proc/sys/net/ipv4/tcp_congestion_control", "bbr");
        LOG_DEBUG("TCP congestion control set to BBR");
    }

    LOG_INFO("TCP stack tuning complete");
    return 0;
}

int routing_get_active_interfaces(char *ifaces, size_t size) {
    if (!ifaces || size == 0) return ATP_ERR_INVAL;

    struct ifaddrs *ifaddr, *ifa;
    char *ptr = ifaces;
    size_t remaining = size;
    int first = 1;
    char seen[64][IFNAMSIZ];
    int seen_count = 0;

    if (getifaddrs(&ifaddr) < 0) {
        LOG_ERROR("getifaddrs failed: %s", strerror(errno));
        return ATP_ERR_IO;
    }

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (!ifa->ifa_name) continue;
        if (!(ifa->ifa_flags & IFF_UP)) continue;
        if (strcmp(ifa->ifa_name, "lo") == 0) continue;

        int already_seen = 0;
        for (int i = 0; i < seen_count; i++) {
            if (strcmp(seen[i], ifa->ifa_name) == 0) {
                already_seen = 1;
                break;
            }
        }
        if (already_seen) continue;

        if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET) {
            char addr[INET_ADDRSTRLEN];
            struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
            inet_ntop(AF_INET, &sin->sin_addr, addr, sizeof(addr));

            if (seen_count < 64) {
                strncpy(seen[seen_count], ifa->ifa_name, IFNAMSIZ - 1);
                seen[seen_count][IFNAMSIZ - 1] = '\0';
                seen_count++;
            }

            if (!first) {
                if (remaining < 2) break;
                *ptr++ = ' ';
                remaining--;
            }
            first = 0;

            int n = snprintf(ptr, remaining, "%s:%s", ifa->ifa_name, addr);
            if (n < 0 || (size_t)n >= remaining) break;
            ptr += n;
            remaining -= n;
        }
    }

    freeifaddrs(ifaddr);
    return ATP_OK;
}

int routing_get_ipv4_addrs(const char *iface, char *output, size_t size) {
    if (!iface || !*iface || !output || size == 0) return ATP_ERR_INVAL;

    if (validate_iface_arg(iface) != ATP_OK) {
        LOG_ERROR("Invalid interface: %s", iface);
        return ATP_ERR_INVAL;
    }

    struct ifaddrs *ifaddr, *ifa;
    char *ptr = output;
    size_t remaining = size;
    int first = 1;
    int found = 0;

    if (getifaddrs(&ifaddr) < 0) {
        LOG_ERROR("getifaddrs failed: %s", strerror(errno));
        return ATP_ERR_IO;
    }

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (!ifa->ifa_name || strcmp(ifa->ifa_name, iface) != 0) continue;
        if (!(ifa->ifa_flags & IFF_UP)) continue;

        if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET) {
            char addr[INET_ADDRSTRLEN];
            struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
            inet_ntop(AF_INET, &sin->sin_addr, addr, sizeof(addr));

            struct sockaddr_in *netmask = (struct sockaddr_in *)ifa->ifa_netmask;
            int prefix = 32;
            if (netmask) {
                uint32_t mask = ntohl(netmask->sin_addr.s_addr);
                prefix = 0;
                while (mask & 0x80000000) { prefix++; mask <<= 1; }
            }

            if (!first) {
                if (remaining < 2) break;
                *ptr++ = ' ';
                remaining--;
            }
            first = 0;
            found = 1;

            int n = snprintf(ptr, remaining, "%s/%d", addr, prefix);
            if (n < 0 || (size_t)n >= remaining) break;
            ptr += n;
            remaining -= n;
        }
    }

    freeifaddrs(ifaddr);

    if (!found) {
        output[0] = '\0';
        return ATP_ERR_NOENT;
    }

    return ATP_OK;
}
