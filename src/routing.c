#include "routing.h"
#include "logger.h"
#include "utils.h"
#include "config.h"
#include "atp.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define IP_CMD "/usr/sbin/ip"

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
    if (family == 4) return exec_ip(cfg, "rule add", rule);
    else return exec_ip6(cfg, "rule add", rule);
}

int routing_rule_del(atp_config_t *cfg, int family, const char *rule) {
    if (family == 4) return exec_ip(cfg, "rule del", rule);
    else return exec_ip6(cfg, "rule del", rule);
}

int routing_rule_del_by_pref(atp_config_t *cfg, int family, int pref) {
    char rule_buf[64];
    snprintf(rule_buf, sizeof(rule_buf), "pref %d", pref);
    return routing_rule_del(cfg, family, rule_buf);
}

int routing_route_add(atp_config_t *cfg, int family, const char *route) {
    if (family == 4) return exec_ip(cfg, "route add", route);
    else return exec_ip6(cfg, "route add", route);
}

int routing_route_del(atp_config_t *cfg, int family, const char *route) {
    if (family == 4) return exec_ip(cfg, "route del", route);
    else return exec_ip6(cfg, "route del", route);
}

int routing_setup_ipv4(atp_config_t *cfg) {
    LOG_INFO("Setting up IPv4 policy routing (table=%d, mark=%d)", 
             cfg->table_id, cfg->mark_value);
    
    char rule_buf[128];
    snprintf(rule_buf, sizeof(rule_buf), "fwmark 0x%x table %d pref %d",
             cfg->mark_value, cfg->table_id, cfg->table_id);
    routing_rule_add(cfg, 4, rule_buf);
    
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
    
    char rule_buf[128];
    snprintf(rule_buf, sizeof(rule_buf), "fwmark 0x%x table %d pref %d",
             cfg->mark_value6, cfg->table_id, cfg->table_id);
    routing_rule_add(cfg, 6, rule_buf);
    
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
    
    routing_rule_del_by_pref(cfg, 4, cfg->table_id);
    
    char route_buf[128];
    snprintf(route_buf, sizeof(route_buf), "local 0.0.0.0/0 dev lo table %d",
             cfg->table_id);
    routing_route_del(cfg, 4, route_buf);
    
    routing_ip_forward_enable(cfg, 0);
    
    LOG_INFO("IPv4 routing cleanup complete");
    return 0;
}

int routing_cleanup_ipv6(atp_config_t *cfg) {
    LOG_INFO("Cleaning up IPv6 policy routing");
    
    routing_rule_del_by_pref(cfg, 6, cfg->table_id);
    
    char route_buf[128];
    snprintf(route_buf, sizeof(route_buf), "local ::/0 dev lo table %d",
             cfg->table_id);
    routing_route_del(cfg, 6, route_buf);
    
    routing_ipv6_forward_enable(cfg, 0);
    
    LOG_INFO("IPv6 routing cleanup complete");
    return 0;
}

int routing_cleanup_all(atp_config_t *cfg) {
    routing_cleanup_ipv4(cfg);
    routing_cleanup_ipv6(cfg);
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
