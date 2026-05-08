#include "routing.h"
#include "logger.h"
#include "utils.h"
#include "config.h"
#include "atp.h"
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>

#define IP_CMD "/system/bin/ip"

static int exec_ip(atp_config_t *cfg, const char *cmd, const char *arg) {
    if (cfg->dry_run) {
        LOG_DEBUG("[DRY_RUN] ip %s %s", cmd, arg ? arg : "");
        return 0;
    }
    
    char command[MAX_CMD_LEN];
    if (arg) {
        snprintf(command, sizeof(command), "%s %s %s 2>/dev/null", IP_CMD, cmd, arg);
    } else {
        snprintf(command, sizeof(command), "%s %s 2>/dev/null", IP_CMD, cmd);
    }
    
    return exec_cmd_simple(command, CMD_TIMEOUT_SEC);
}

static int exec_ip6(atp_config_t *cfg, const char *cmd, const char *arg) {
    if (cfg->dry_run) {
        LOG_DEBUG("[DRY_RUN] ip -6 %s %s", cmd, arg ? arg : "");
        return 0;
    }
    
    char command[MAX_CMD_LEN];
    if (arg) {
        snprintf(command, sizeof(command), "%s -6 %s %s 2>/dev/null", IP_CMD, cmd, arg);
    } else {
        snprintf(command, sizeof(command), "%s -6 %s 2>/dev/null", IP_CMD, cmd);
    }
    
    return exec_cmd_simple(command, CMD_TIMEOUT_SEC);
}

int routing_rule_add(atp_config_t *cfg, int family, const char *rule) {
    if (family == 4) {
        return exec_ip(cfg, "rule add", rule);
    } else {
        return exec_ip6(cfg, "rule add", rule);
    }
}

int routing_rule_del(atp_config_t *cfg, int family, const char *rule) {
    if (family == 4) {
        return exec_ip(cfg, "rule del", rule);
    } else {
        return exec_ip6(cfg, "rule del", rule);
    }
}

int routing_rule_del_by_pref(atp_config_t *cfg, int family, int pref) {
    char rule_buf[64];
    snprintf(rule_buf, sizeof(rule_buf), "pref %d", pref);
    return routing_rule_del(cfg, family, rule_buf);
}

int routing_rule_del_all_by_pref(atp_config_t *cfg, int family, int pref) {
    int count = 0;
    char check_buf[256];
    char output[256];
    
    if (cfg->dry_run) {
        LOG_DEBUG("[DRY_RUN] Would delete all rules with pref %d", pref);
        return 0;
    }
    
    if (family == 4) {
        snprintf(check_buf, sizeof(check_buf), "%s rule show | grep 'pref %d'", IP_CMD, pref);
    } else {
        snprintf(check_buf, sizeof(check_buf), "%s -6 rule show | grep 'pref %d'", IP_CMD, pref);
    }
    
    while (1) {
        if (exec_cmd(check_buf, output, sizeof(output), 5) != 0) break;
        if (output[0] == '\0') break;
        
        routing_rule_del_by_pref(cfg, family, pref);
        count++;
        
        if (count > 100) break;
    }
    
    return count;
}

int routing_route_add(atp_config_t *cfg, int family, const char *route) {
    if (family == 4) {
        return exec_ip(cfg, "route add", route);
    } else {
        return exec_ip6(cfg, "route add", route);
    }
}

int routing_route_del(atp_config_t *cfg, int family, const char *route) {
    if (family == 4) {
        return exec_ip(cfg, "route del", route);
    } else {
        return exec_ip6(cfg, "route del", route);
    }
}

int routing_route_flush_table(atp_config_t *cfg, int family, int table_id) {
    char flush_buf[64];
    snprintf(flush_buf, sizeof(flush_buf), "table %d", table_id);
    
    if (family == 4) {
        return exec_ip(cfg, "route flush", flush_buf);
    } else {
        return exec_ip6(cfg, "route flush", flush_buf);
    }
}

/* P1: Use $ anchor for precise table_id match, prevent substring false positive */
static int routing_rule_exists(atp_config_t *cfg, int family, int mark, int table_id) {
    char cmd[MAX_CMD_LEN];
    char output[256];
    
    if (cfg->dry_run) {
        LOG_DEBUG("[DRY_RUN] Check routing rule exists: fwmark 0x%x table %d", mark, table_id);
        return 0;
    }
    
    if (family == 4) {
        snprintf(cmd, sizeof(cmd), 
                 "%s rule show | grep -q 'fwmark 0x%x.*lookup %d '", IP_CMD, mark, table_id);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "%s -6 rule show | grep -q 'fwmark 0x%x.*lookup %d '", IP_CMD, mark, table_id);
    }
    
    return exec_cmd(cmd, output, sizeof(output), 5) == 0;
}

int routing_setup_ipv4(atp_config_t *cfg) {
    LOG_INFO("Setting up IPv4 policy routing (table=%d, mark=%d)", 
             cfg->table_id, cfg->mark_value);
    
    routing_rule_del_all_by_pref(cfg, 4, cfg->table_id);
    
    if (!routing_rule_exists(cfg, 4, cfg->mark_value, cfg->table_id)) {
        char rule_buf[128];
        snprintf(rule_buf, sizeof(rule_buf), "fwmark 0x%x table %d pref %d",
                 cfg->mark_value, cfg->table_id, cfg->table_id);
        routing_rule_add(cfg, 4, rule_buf);
        LOG_DEBUG("Added IPv4 routing rule");
    } else {
        LOG_DEBUG("IPv4 routing rule already exists, skipping");
    }
    
    char route_buf[128];
    snprintf(route_buf, sizeof(route_buf), "local 0.0.0.0/0 dev lo table %d",
             cfg->table_id);
    routing_route_add(cfg, 4, route_buf);
    
    routing_ip_forward_enable(cfg, 1);
    
    LOG_INFO("IPv4 routing setup complete");
    return 0;
}

int routing_setup_ipv6(atp_config_t *cfg) {
    if (!cfg->proxy_ipv6) {
        LOG_DEBUG("IPv6 proxy disabled, skipping routing");
        return 0;
    }
    
    LOG_INFO("Setting up IPv6 policy routing (table=%d, mark=%d)", 
             cfg->table_id, cfg->mark_value6);
    
    routing_rule_del_all_by_pref(cfg, 6, cfg->table_id);
    
    if (!routing_rule_exists(cfg, 6, cfg->mark_value6, cfg->table_id)) {
        char rule_buf[128];
        snprintf(rule_buf, sizeof(rule_buf), "fwmark 0x%x table %d pref %d",
                 cfg->mark_value6, cfg->table_id, cfg->table_id);
        routing_rule_add(cfg, 6, rule_buf);
        LOG_DEBUG("Added IPv6 routing rule");
    } else {
        LOG_DEBUG("IPv6 routing rule already exists, skipping");
    }
    
    char route_buf[128];
    snprintf(route_buf, sizeof(route_buf), "local ::/0 dev lo table %d",
             cfg->table_id);
    routing_route_add(cfg, 6, route_buf);
    
    routing_ipv6_forward_enable(cfg, 1);
    
    LOG_INFO("IPv6 routing setup complete");
    return 0;
}

int routing_cleanup_ipv4(atp_config_t *cfg) {
    LOG_INFO("Cleaning up IPv4 policy routing");
    
    routing_rule_del_all_by_pref(cfg, 4, cfg->table_id);
    
    char route_buf[128];
    snprintf(route_buf, sizeof(route_buf), "local 0.0.0.0/0 dev lo table %d",
             cfg->table_id);
    routing_route_del(cfg, 4, route_buf);
    
    routing_route_flush_table(cfg, 4, cfg->table_id);
    
    routing_ip_forward_enable(cfg, 0);
    
    LOG_INFO("IPv4 routing cleanup complete");
    return 0;
}

int routing_cleanup_ipv6(atp_config_t *cfg) {
    LOG_INFO("Cleaning up IPv6 policy routing");
    
    routing_rule_del_all_by_pref(cfg, 6, cfg->table_id);
    
    char route_buf[128];
    snprintf(route_buf, sizeof(route_buf), "local ::/0 dev lo table %d",
             cfg->table_id);
    routing_route_del(cfg, 6, route_buf);
    
    routing_route_flush_table(cfg, 6, cfg->table_id);
    
    routing_ipv6_forward_enable(cfg, 0);
    
    LOG_INFO("IPv6 routing cleanup complete");
    return 0;
}

int routing_cleanup_all(atp_config_t *cfg) {
    routing_cleanup_ipv4(cfg);
    routing_cleanup_ipv6(cfg);
    return 0;
}

int routing_add_vpn_policy(atp_config_t *cfg, const char *vpn_iface) {
    if (!vpn_iface || !vpn_iface[0]) return -1;
    
    LOG_INFO("Adding VPN policy for interface: %s", vpn_iface);
    
    char rule_buf[128];
    snprintf(rule_buf, sizeof(rule_buf), "fwmark 0x20000 table %d pref 20000", cfg->table_id);
    routing_rule_add(cfg, 4, rule_buf);
    LOG_DEBUG("Added global fwmark lock (pref 20000)");
    
    if (cfg->proxy_ipv6) {
        snprintf(rule_buf, sizeof(rule_buf), "fwmark 0x20000 table %d pref 20000", cfg->table_id);
        routing_rule_add(cfg, 6, rule_buf);
        LOG_DEBUG("Added IPv6 global fwmark lock");
    }
    
    snprintf(rule_buf, sizeof(rule_buf), "from all iif ap0 lookup %s pref 100", vpn_iface);
    routing_rule_add(cfg, 4, rule_buf);
    LOG_DEBUG("Added hotspot policy (iif ap0 -> %s)", vpn_iface);
    
    if (cfg->proxy_ipv6) {
        snprintf(rule_buf, sizeof(rule_buf), "from all iif ap0 lookup %s pref 100", vpn_iface);
        routing_rule_add(cfg, 6, rule_buf);
        LOG_DEBUG("Added IPv6 hotspot policy");
    }
    
    strncpy(cfg->current_vpn_iface, vpn_iface, sizeof(cfg->current_vpn_iface) - 1);
    cfg->current_vpn_iface[sizeof(cfg->current_vpn_iface) - 1] = '\0';
    
    LOG_INFO("VPN policy added successfully");
    return 0;
}

int routing_remove_vpn_policy(atp_config_t *cfg, const char *vpn_iface) {
    if (!vpn_iface || !vpn_iface[0]) {
        vpn_iface = cfg->current_vpn_iface;
    }
    
    if (!vpn_iface || !vpn_iface[0]) return 0;
    
    LOG_INFO("Removing VPN policy for interface: %s", vpn_iface);
    
    routing_rule_del_by_pref(cfg, 4, 20000);
    LOG_DEBUG("Removed global fwmark lock");
    
    if (cfg->proxy_ipv6) {
        routing_rule_del_by_pref(cfg, 6, 20000);
        LOG_DEBUG("Removed IPv6 global fwmark lock");
    }
    
    routing_rule_del_by_pref(cfg, 4, 100);
    LOG_DEBUG("Removed hotspot policy");
    
    if (cfg->proxy_ipv6) {
        routing_rule_del_by_pref(cfg, 6, 100);
        LOG_DEBUG("Removed IPv6 hotspot policy");
    }
    
    cfg->current_vpn_iface[0] = '\0';
    
    LOG_INFO("VPN policy removed successfully");
    return 0;
}

int routing_add_mss_clamp(atp_config_t *cfg, const char *iface) {
    if (!iface || !iface[0]) return -1;
    
    LOG_INFO("Adding MSS clamp for interface: %s", iface);
    
    if (cfg->dry_run) {
        LOG_DEBUG("[DRY_RUN] iptables -t mangle -A FORWARD -o %s -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --clamp-mss-to-pmtu", iface);
        return 0;
    }
    
    char rule_buf[256];
    snprintf(rule_buf, sizeof(rule_buf),
             "-o %s -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --clamp-mss-to-pmtu",
             iface);
    
    char cmd[MAX_CMD_LEN];
    snprintf(cmd, sizeof(cmd), 
             "/system/bin/iptables -t mangle -C FORWARD %s 2>/dev/null", rule_buf);
    
    if (exec_cmd_simple(cmd, CMD_TIMEOUT_SEC) != 0) {
        snprintf(cmd, sizeof(cmd), 
                 "/system/bin/iptables -t mangle -A FORWARD %s", rule_buf);
        exec_cmd_simple(cmd, CMD_TIMEOUT_SEC);
        LOG_DEBUG("MSS clamp rule added");
    } else {
        LOG_DEBUG("MSS clamp rule already exists");
    }
    
    return 0;
}

int routing_remove_mss_clamp(atp_config_t *cfg, const char *iface) {
    if (!iface || !iface[0]) return 0;
    
    LOG_INFO("Removing MSS clamp for interface: %s", iface);
    
    if (cfg->dry_run) {
        LOG_DEBUG("[DRY_RUN] iptables -t mangle -D FORWARD -o %s -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --clamp-mss-to-pmtu", iface);
        return 0;
    }
    
    char rule_buf[256];
    snprintf(rule_buf, sizeof(rule_buf),
             "-o %s -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --clamp-mss-to-pmtu",
             iface);
    
    char cmd[MAX_CMD_LEN];
    snprintf(cmd, sizeof(cmd), 
             "/system/bin/iptables -t mangle -D FORWARD %s 2>/dev/null", rule_buf);
    exec_cmd_simple(cmd, CMD_TIMEOUT_SEC);
    
    LOG_DEBUG("MSS clamp rule removed");
    return 0;
}

int routing_ip_forward_enable(atp_config_t *cfg, int enable) {
    const char *path = "/proc/sys/net/ipv4/ip_forward";
    
    if (cfg->dry_run) {
        LOG_DEBUG("[DRY_RUN] echo %d > %s", enable, path);
        return 0;
    }
    
    FILE *fp = fopen(path, "w");
    if (!fp) {
        LOG_ERROR("Failed to open %s: %s", path, strerror(errno));
        return -1;
    }
    
    fprintf(fp, "%d\n", enable);
    fclose(fp);
    
    LOG_DEBUG("IPv4 forwarding set to %d", enable);
    return 0;
}

int routing_ipv6_forward_enable(atp_config_t *cfg, int enable) {
    const char *path = "/proc/sys/net/ipv6/conf/all/forwarding";
    
    if (cfg->dry_run) {
        LOG_DEBUG("[DRY_RUN] echo %d > %s", enable, path);
        return 0;
    }
    
    FILE *fp = fopen(path, "w");
    if (!fp) {
        LOG_WARN("Failed to open %s: %s", path, strerror(errno));
        return -1;
    }
    
    fprintf(fp, "%d\n", enable);
    fclose(fp);
    
    LOG_DEBUG("IPv6 forwarding set to %d", enable);
    return 0;
}

int routing_rp_filter_set(atp_config_t *cfg, int value) {
    char path[256];
    DIR *dir;
    struct dirent *entry;
    
    dir = opendir("/proc/sys/net/ipv4/conf");
    if (!dir) return -1;
    
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        
        snprintf(path, sizeof(path), "/proc/sys/net/ipv4/conf/%s/rp_filter", entry->d_name);
        
        if (cfg->dry_run) {
            LOG_DEBUG("[DRY_RUN] echo %d > %s", value, path);
            continue;
        }
        
        FILE *fp = fopen(path, "w");
        if (fp) {
            fprintf(fp, "%d\n", value);
            fclose(fp);
        }
    }
    
    closedir(dir);
    LOG_DEBUG("rp_filter set to %d for all interfaces", value);
    return 0;
}

int routing_tcp_stack_tune(atp_config_t *cfg) {
    if (cfg->dry_run) {
        LOG_DEBUG("[DRY_RUN] TCP stack tuning skipped");
        return 0;
    }
    
    exec_cmd_simple("echo 3 > /proc/sys/net/ipv4/tcp_fastopen 2>/dev/null", 5);
    exec_cmd_simple("echo 16777216 > /proc/sys/net/core/rmem_max 2>/dev/null", 5);
    exec_cmd_simple("echo 16777216 > /proc/sys/net/core/wmem_max 2>/dev/null", 5);
    exec_cmd_simple("echo '4096 87380 16777216' > /proc/sys/net/ipv4/tcp_rmem 2>/dev/null", 5);
    exec_cmd_simple("echo '4096 65536 16777216' > /proc/sys/net/ipv4/tcp_wmem 2>/dev/null", 5);
    
    char output[256];
    exec_cmd("grep -q bbr /proc/sys/net/ipv4/tcp_allowed_congestion_control 2>/dev/null", 
             output, sizeof(output), 5);
    if (output[0] == '\0') {
        exec_cmd_simple("echo bbr > /proc/sys/net/ipv4/tcp_congestion_control 2>/dev/null", 5);
        LOG_DEBUG("TCP congestion control set to BBR");
    }
    
    LOG_INFO("TCP stack tuning complete");
    return 0;
}

int routing_get_active_interfaces(char *ifaces, size_t size) {
    char cmd[MAX_CMD_LEN];
    snprintf(cmd, sizeof(cmd), 
             "%s -brief addr show | awk '$3 != \"\" {print $1\":\"$3}' | grep -v lo: | tr '\\n' ' '",
             IP_CMD);
    
    return exec_cmd(cmd, ifaces, size, 5);
}

int routing_get_ipv4_addrs(const char *iface, char *output, size_t size) {
    char cmd[MAX_CMD_LEN];
    snprintf(cmd, sizeof(cmd), 
             "%s -4 addr show dev %s | grep 'inet ' | awk '{print $2}' | tr '\\n' ' '",
             IP_CMD, iface);
    
    return exec_cmd(cmd, output, size, 5);
}
