#include "tproxy.h"
#include "logger.h"
#include "utils.h"
#include "config.h"
#include "atp.h"
#include <stdlib.h>
#include <string.h>

#define IPTABLES_CMD "/system/bin/iptables"
#define IP6TABLES_CMD "/system/bin/ip6tables"

static int exec_iptables(atp_config_t *cfg, const char *table, const char *cmd, const char *chain, const char *rule) {
    if (cfg->dry_run) {
        LOG_DEBUG("[DRY_RUN] iptables -t %s %s %s %s", table, cmd, chain, rule ? rule : "");
        return 0;
    }
    
    char command[MAX_CMD_LEN];
    if (rule) {
        snprintf(command, sizeof(command), "%s -t %s %s %s %s 2>/dev/null",
                 IPTABLES_CMD, table, cmd, chain, rule);
    } else {
        snprintf(command, sizeof(command), "%s -t %s %s %s 2>/dev/null",
                 IPTABLES_CMD, table, cmd, chain);
    }
    
    return exec_cmd_simple(command, CMD_TIMEOUT_SEC);
}

static int exec_ip6tables(atp_config_t *cfg, const char *table, const char *cmd, const char *chain, const char *rule) {
    if (cfg->dry_run) {
        LOG_DEBUG("[DRY_RUN] ip6tables -t %s %s %s %s", table, cmd, chain, rule ? rule : "");
        return 0;
    }
    
    char command[MAX_CMD_LEN];
    if (rule) {
        snprintf(command, sizeof(command), "%s -t %s %s %s %s 2>/dev/null",
                 IP6TABLES_CMD, table, cmd, chain, rule);
    } else {
        snprintf(command, sizeof(command), "%s -t %s %s %s 2>/dev/null",
                 IP6TABLES_CMD, table, cmd, chain);
    }
    
    return exec_cmd_simple(command, CMD_TIMEOUT_SEC);
}

int tproxy_chain_create(atp_config_t *cfg, int family, const char *table, const char *chain) {
    if (family == 4) {
        return exec_iptables(cfg, table, "-N", chain, NULL);
    } else {
        return exec_ip6tables(cfg, table, "-N", chain, NULL);
    }
}

int tproxy_chain_flush(atp_config_t *cfg, int family, const char *table, const char *chain) {
    if (family == 4) {
        return exec_iptables(cfg, table, "-F", chain, NULL);
    } else {
        return exec_ip6tables(cfg, table, "-F", chain, NULL);
    }
}

int tproxy_chain_destroy(atp_config_t *cfg, int family, const char *table, const char *chain) {
    if (family == 4) {
        return exec_iptables(cfg, table, "-X", chain, NULL);
    } else {
        return exec_ip6tables(cfg, table, "-X", chain, NULL);
    }
}

int tproxy_chain_exists(atp_config_t *cfg, int family, const char *table, const char *chain) {
    char check_cmd[MAX_CMD_LEN];
    
    if (family == 4) {
        snprintf(check_cmd, sizeof(check_cmd), "%s -t %s -L %s 2>/dev/null | head -1",
                 IPTABLES_CMD, table, chain);
    } else {
        snprintf(check_cmd, sizeof(check_cmd), "%s -t %s -L %s 2>/dev/null | head -1",
                 IP6TABLES_CMD, table, chain);
    }
    
    char output[256];
    if (exec_cmd(check_cmd, output, sizeof(output), 5) == 0 && output[0] != '\0') {
        return 1;
    }
    return 0;
}

int tproxy_rule_add(atp_config_t *cfg, int family, const char *table,
                    const char *chain, const char *rule) {
    if (family == 4) {
        return exec_iptables(cfg, table, "-A", chain, rule);
    } else {
        return exec_ip6tables(cfg, table, "-A", chain, rule);
    }
}

int tproxy_rule_del(atp_config_t *cfg, int family, const char *table,
                    const char *chain, const char *rule) {
    if (family == 4) {
        return exec_iptables(cfg, table, "-D", chain, rule);
    } else {
        return exec_ip6tables(cfg, table, "-D", chain, rule);
    }
}

int tproxy_rule_insert(atp_config_t *cfg, int family, const char *table,
                       const char *chain, int position, const char *rule) {
    char pos_str[16];
    snprintf(pos_str, sizeof(pos_str), "-I %s %d", chain, position);
    
    if (family == 4) {
        char cmd[MAX_CMD_LEN];
        snprintf(cmd, sizeof(cmd), "%s -t %s -I %s %d %s",
                 IPTABLES_CMD, table, chain, position, rule);
        return exec_cmd_simple(cmd, CMD_TIMEOUT_SEC);
    } else {
        char cmd[MAX_CMD_LEN];
        snprintf(cmd, sizeof(cmd), "%s -t %s -I %s %d %s",
                 IP6TABLES_CMD, table, chain, position, rule);
        return exec_cmd_simple(cmd, CMD_TIMEOUT_SEC);
    }
}

int tproxy_atomic_switch(atp_config_t *cfg, int family, const char *table,
                         const char *hook, const char *chain0, const char *chain1) {
    char rule_buf[256];
    
    snprintf(rule_buf, sizeof(rule_buf), "-j %s", chain1);
    
    if (tproxy_rule_del(cfg, family, table, hook, rule_buf) != 0) {
        snprintf(rule_buf, sizeof(rule_buf), "-j %s", chain0);
        tproxy_rule_del(cfg, family, table, hook, rule_buf);
    }
    
    snprintf(rule_buf, sizeof(rule_buf), "-j %s", chain1);
    return tproxy_rule_insert(cfg, family, table, hook, 1, rule_buf);
}

int tproxy_support_check(atp_config_t *cfg) {
    if (cfg->dry_run) return 1;
    
    char cmd[MAX_CMD_LEN];
    snprintf(cmd, sizeof(cmd), "%s -t mangle -N ATP_TEST 2>/dev/null", IPTABLES_CMD);
    exec_cmd_simple(cmd, 5);
    
    snprintf(cmd, sizeof(cmd), "%s -t mangle -A ATP_TEST -p tcp -j TPROXY --on-port 1536 --tproxy-mark 20 2>/dev/null",
             IPTABLES_CMD);
    int ret = exec_cmd_simple(cmd, 5);
    
    exec_cmd_simple(IPTABLES_CMD " -t mangle -F ATP_TEST 2>/dev/null", 5);
    exec_cmd_simple(IPTABLES_CMD " -t mangle -X ATP_TEST 2>/dev/null", 5);
    
    return ret == 0;
}

int tproxy_setup_ipv4(atp_config_t *cfg) {
    LOG_INFO("Setting up IPv4 TPROXY chains");
    
    const char *chains[] = {
        "ATP_PRE_0", "ATP_PRE_1",
        "ATP_OUT_0", "ATP_OUT_1",
        "ATP_DIVERT_0", "ATP_DIVERT_1",
        "ATP_PROXY_IP_0", "ATP_PROXY_IP_1",
        "ATP_BYPASS_IP_0", "ATP_BYPASS_IP_1",
        "ATP_PROXY_IFACE_0", "ATP_PROXY_IFACE_1",
        "ATP_BYPASS_IFACE_0", "ATP_BYPASS_IFACE_1",
        "ATP_DNS_PRE_0", "ATP_DNS_PRE_1",
        "ATP_DNS_OUT_0", "ATP_DNS_OUT_1",
        "ATP_APP_0", "ATP_APP_1",
        "ATP_MAC_0", "ATP_MAC_1",
        NULL
    };
    
    for (int i = 0; chains[i] != NULL; i++) {
        if (!tproxy_chain_exists(cfg, 4, "mangle", chains[i])) {
            tproxy_chain_create(cfg, 4, "mangle", chains[i]);
        }
        tproxy_chain_flush(cfg, 4, "mangle", chains[i]);
    }
    
    tproxy_rule_add(cfg, 4, "mangle", "ATP_PRE_0", "-j ATP_PROXY_IP_0");
    tproxy_rule_add(cfg, 4, "mangle", "ATP_PRE_0", "-j ATP_BYPASS_IP_0");
    tproxy_rule_add(cfg, 4, "mangle", "ATP_PRE_0", "-j ATP_PROXY_IFACE_0");
    tproxy_rule_add(cfg, 4, "mangle", "ATP_PRE_0", "-j ATP_MAC_0");
    tproxy_rule_add(cfg, 4, "mangle", "ATP_PRE_0", "-j ATP_DNS_PRE_0");
    
    tproxy_rule_add(cfg, 4, "mangle", "ATP_OUT_0", "-j ATP_PROXY_IP_0");
    tproxy_rule_add(cfg, 4, "mangle", "ATP_OUT_0", "-j ATP_BYPASS_IP_0");
    tproxy_rule_add(cfg, 4, "mangle", "ATP_OUT_0", "-j ATP_BYPASS_IFACE_0");
    tproxy_rule_add(cfg, 4, "mangle", "ATP_OUT_0", "-j ATP_APP_0");
    tproxy_rule_add(cfg, 4, "mangle", "ATP_OUT_0", "-j ATP_DNS_OUT_0");
    
    char hook_rule[64];
    snprintf(hook_rule, sizeof(hook_rule), "-j ATP_PRE_0");
    tproxy_rule_insert(cfg, 4, "mangle", "PREROUTING", 1, hook_rule);
    
    snprintf(hook_rule, sizeof(hook_rule), "-j ATP_OUT_0");
    tproxy_rule_insert(cfg, 4, "mangle", "OUTPUT", 1, hook_rule);
    
    LOG_INFO("IPv4 TPROXY setup complete");
    return 0;
}

int tproxy_setup_ipv6(atp_config_t *cfg) {
    if (!cfg->proxy_ipv6) {
        LOG_DEBUG("IPv6 proxy disabled, skipping");
        return 0;
    }
    
    LOG_INFO("Setting up IPv6 TPROXY chains");
    
    const char *chains[] = {
        "ATP6_PRE_0", "ATP6_PRE_1",
        "ATP6_OUT_0", "ATP6_OUT_1",
        "ATP6_DIVERT_0", "ATP6_DIVERT_1",
        "ATP6_PROXY_IP_0", "ATP6_PROXY_IP_1",
        "ATP6_BYPASS_IP_0", "ATP6_BYPASS_IP_1",
        "ATP6_PROXY_IFACE_0", "ATP6_PROXY_IFACE_1",
        "ATP6_BYPASS_IFACE_0", "ATP6_BYPASS_IFACE_1",
        "ATP6_DNS_PRE_0", "ATP6_DNS_PRE_1",
        "ATP6_DNS_OUT_0", "ATP6_DNS_OUT_1",
        "ATP6_APP_0", "ATP6_APP_1",
        "ATP6_MAC_0", "ATP6_MAC_1",
        NULL
    };
    
    for (int i = 0; chains[i] != NULL; i++) {
        if (!tproxy_chain_exists(cfg, 6, "mangle", chains[i])) {
            tproxy_chain_create(cfg, 6, "mangle", chains[i]);
        }
        tproxy_chain_flush(cfg, 6, "mangle", chains[i]);
    }
    
    tproxy_rule_add(cfg, 6, "mangle", "ATP6_PRE_0", "-j ATP6_PROXY_IP_0");
    tproxy_rule_add(cfg, 6, "mangle", "ATP6_PRE_0", "-j ATP6_BYPASS_IP_0");
    tproxy_rule_add(cfg, 6, "mangle", "ATP6_PRE_0", "-j ATP6_PROXY_IFACE_0");
    tproxy_rule_add(cfg, 6, "mangle", "ATP6_PRE_0", "-j ATP6_DNS_PRE_0");
    
    tproxy_rule_add(cfg, 6, "mangle", "ATP6_OUT_0", "-j ATP6_PROXY_IP_0");
    tproxy_rule_add(cfg, 6, "mangle", "ATP6_OUT_0", "-j ATP6_BYPASS_IP_0");
    tproxy_rule_add(cfg, 6, "mangle", "ATP6_OUT_0", "-j ATP6_BYPASS_IFACE_0");
    tproxy_rule_add(cfg, 6, "mangle", "ATP6_OUT_0", "-j ATP6_DNS_OUT_0");
    
    char hook_rule[64];
    snprintf(hook_rule, sizeof(hook_rule), "-j ATP6_PRE_0");
    tproxy_rule_insert(cfg, 6, "mangle", "PREROUTING", 1, hook_rule);
    
    snprintf(hook_rule, sizeof(hook_rule), "-j ATP6_OUT_0");
    tproxy_rule_insert(cfg, 6, "mangle", "OUTPUT", 1, hook_rule);
    
    LOG_INFO("IPv6 TPROXY setup complete");
    return 0;
}

int tproxy_cleanup_ipv4(atp_config_t *cfg) {
    LOG_INFO("Cleaning up IPv4 TPROXY chains");
    
    tproxy_rule_del(cfg, 4, "mangle", "PREROUTING", "-j ATP_PRE_0");
    tproxy_rule_del(cfg, 4, "mangle", "OUTPUT", "-j ATP_OUT_0");
    
    const char *chains[] = {
        "ATP_PRE_0", "ATP_PRE_1",
        "ATP_OUT_0", "ATP_OUT_1",
        "ATP_DIVERT_0", "ATP_DIVERT_1",
        "ATP_PROXY_IP_0", "ATP_PROXY_IP_1",
        "ATP_BYPASS_IP_0", "ATP_BYPASS_IP_1",
        "ATP_PROXY_IFACE_0", "ATP_PROXY_IFACE_1",
        "ATP_BYPASS_IFACE_0", "ATP_BYPASS_IFACE_1",
        "ATP_DNS_PRE_0", "ATP_DNS_PRE_1",
        "ATP_DNS_OUT_0", "ATP_DNS_OUT_1",
        "ATP_APP_0", "ATP_APP_1",
        "ATP_MAC_0", "ATP_MAC_1",
        NULL
    };
    
    for (int i = 0; chains[i] != NULL; i++) {
        tproxy_chain_flush(cfg, 4, "mangle", chains[i]);
        tproxy_chain_destroy(cfg, 4, "mangle", chains[i]);
    }
    
    LOG_INFO("IPv4 TPROXY cleanup complete");
    return 0;
}

int tproxy_cleanup_ipv6(atp_config_t *cfg) {
    LOG_INFO("Cleaning up IPv6 TPROXY chains");
    
    tproxy_rule_del(cfg, 6, "mangle", "PREROUTING", "-j ATP6_PRE_0");
    tproxy_rule_del(cfg, 6, "mangle", "OUTPUT", "-j ATP6_OUT_0");
    
    const char *chains[] = {
        "ATP6_PRE_0", "ATP6_PRE_1",
        "ATP6_OUT_0", "ATP6_OUT_1",
        "ATP6_DIVERT_0", "ATP6_DIVERT_1",
        "ATP6_PROXY_IP_0", "ATP6_PROXY_IP_1",
        "ATP6_BYPASS_IP_0", "ATP6_BYPASS_IP_1",
        "ATP6_PROXY_IFACE_0", "ATP6_PROXY_IFACE_1",
        "ATP6_BYPASS_IFACE_0", "ATP6_BYPASS_IFACE_1",
        "ATP6_DNS_PRE_0", "ATP6_DNS_PRE_1",
        "ATP6_DNS_OUT_0", "ATP6_DNS_OUT_1",
        "ATP6_APP_0", "ATP6_APP_1",
        "ATP6_MAC_0", "ATP6_MAC_1",
        NULL
    };
    
    for (int i = 0; chains[i] != NULL; i++) {
        tproxy_chain_flush(cfg, 6, "mangle", chains[i]);
        tproxy_chain_destroy(cfg, 6, "mangle", chains[i]);
    }
    
    LOG_INFO("IPv6 TPROXY cleanup complete");
    return 0;
}

int tproxy_cleanup_all(atp_config_t *cfg) {
    tproxy_cleanup_ipv4(cfg);
    tproxy_cleanup_ipv6(cfg);
    return 0;
}

int tproxy_dns_hijack_setup(atp_config_t *cfg, int family, int mode) {
    if (cfg->dns_hijack == DNS_HIJACK_OFF) return 0;
    if (mode == DNS_HIJACK_OFF) return 0;
    
    LOG_INFO("Setting up DNS hijack for IPv%d (mode=%d)", family, mode);
    
    const char *dns_rule = NULL;
    char rule_buf[128];
    
    if (mode == DNS_HIJACK_TPROXY) {
        snprintf(rule_buf, sizeof(rule_buf), 
                 "-p udp --dport 53 -j TPROXY --on-port %d --tproxy-mark %d",
                 cfg->dns_port, cfg->mark_value);
        dns_rule = rule_buf;
    } else if (mode == DNS_HIJACK_REDIRECT) {
        snprintf(rule_buf, sizeof(rule_buf),
                 "-p udp --dport 53 -j REDIRECT --to-ports %d",
                 cfg->dns_port);
        dns_rule = rule_buf;
    }
    
    if (dns_rule) {
        if (family == 4) {
            tproxy_rule_add(cfg, 4, "mangle", "ATP_DNS_PRE_0", dns_rule);
            tproxy_rule_add(cfg, 4, "mangle", "ATP_DNS_OUT_0", dns_rule);
        } else {
            tproxy_rule_add(cfg, 6, "mangle", "ATP6_DNS_PRE_0", dns_rule);
            tproxy_rule_add(cfg, 6, "mangle", "ATP6_DNS_OUT_0", dns_rule);
        }
    }
    
    return 0;
}

int tproxy_dns_hijack_cleanup(atp_config_t *cfg, int family) {
    if (family == 4) {
        tproxy_chain_flush(cfg, 4, "mangle", "ATP_DNS_PRE_0");
        tproxy_chain_flush(cfg, 4, "mangle", "ATP_DNS_OUT_0");
    } else {
        tproxy_chain_flush(cfg, 6, "mangle", "ATP6_DNS_PRE_0");
        tproxy_chain_flush(cfg, 6, "mangle", "ATP6_DNS_OUT_0");
    }
    return 0;
}

int tproxy_block_quic(atp_config_t *cfg, int enable) {
    if (enable) {
        LOG_INFO("Enabling QUIC blocking");
        
        if (!tproxy_chain_exists(cfg, 4, "filter", "ATP_QUIC_0")) {
            tproxy_chain_create(cfg, 4, "filter", "ATP_QUIC_0");
        }
        tproxy_chain_flush(cfg, 4, "filter", "ATP_QUIC_0");
        
        tproxy_rule_add(cfg, 4, "filter", "ATP_QUIC_0", "-p udp --dport 443 -j REJECT");
        tproxy_rule_add(cfg, 4, "filter", "INPUT", "-j ATP_QUIC_0");
        tproxy_rule_add(cfg, 4, "filter", "FORWARD", "-j ATP_QUIC_0");
        tproxy_rule_add(cfg, 4, "filter", "OUTPUT", "-j ATP_QUIC_0");
        
        if (cfg->proxy_ipv6) {
            if (!tproxy_chain_exists(cfg, 6, "filter", "ATP6_QUIC_0")) {
                tproxy_chain_create(cfg, 6, "filter", "ATP6_QUIC_0");
            }
            tproxy_chain_flush(cfg, 6, "filter", "ATP6_QUIC_0");
            tproxy_rule_add(cfg, 6, "filter", "ATP6_QUIC_0", "-p udp --dport 443 -j REJECT");
            tproxy_rule_add(cfg, 6, "filter", "INPUT", "-j ATP6_QUIC_0");
            tproxy_rule_add(cfg, 6, "filter", "FORWARD", "-j ATP6_QUIC_0");
            tproxy_rule_add(cfg, 6, "filter", "OUTPUT", "-j ATP6_QUIC_0");
        }
    } else {
        LOG_INFO("Disabling QUIC blocking");
        
        tproxy_rule_del(cfg, 4, "filter", "INPUT", "-j ATP_QUIC_0");
        tproxy_rule_del(cfg, 4, "filter", "FORWARD", "-j ATP_QUIC_0");
        tproxy_rule_del(cfg, 4, "filter", "OUTPUT", "-j ATP_QUIC_0");
        tproxy_chain_flush(cfg, 4, "filter", "ATP_QUIC_0");
        tproxy_chain_destroy(cfg, 4, "filter", "ATP_QUIC_0");
        
        if (cfg->proxy_ipv6) {
            tproxy_rule_del(cfg, 6, "filter", "INPUT", "-j ATP6_QUIC_0");
            tproxy_rule_del(cfg, 6, "filter", "FORWARD", "-j ATP6_QUIC_0");
            tproxy_rule_del(cfg, 6, "filter", "OUTPUT", "-j ATP6_QUIC_0");
            tproxy_chain_flush(cfg, 6, "filter", "ATP6_QUIC_0");
            tproxy_chain_destroy(cfg, 6, "filter", "ATP6_QUIC_0");
        }
    }
    
    return 0;
}

int tproxy_block_loopback(atp_config_t *cfg, int enable) {
    char rule_buf[256];
    
    if (enable) {
        LOG_INFO("Enabling loopback protection");
        snprintf(rule_buf, sizeof(rule_buf),
                 "-d 127.0.0.1 -p tcp -m tcp --dport %d -j REJECT",
                 cfg->tcp_port);
        tproxy_rule_add(cfg, 4, "filter", "OUTPUT", rule_buf);
        
        if (cfg->proxy_ipv6) {
            snprintf(rule_buf, sizeof(rule_buf),
                     "-d ::1 -p tcp -m tcp --dport %d -j REJECT",
                     cfg->tcp_port);
            tproxy_rule_add(cfg, 6, "filter", "OUTPUT", rule_buf);
        }
    } else {
        LOG_INFO("Disabling loopback protection");
        snprintf(rule_buf, sizeof(rule_buf),
                 "-d 127.0.0.1 -p tcp -m tcp --dport %d -j REJECT",
                 cfg->tcp_port);
        tproxy_rule_del(cfg, 4, "filter", "OUTPUT", rule_buf);
        
        if (cfg->proxy_ipv6) {
            snprintf(rule_buf, sizeof(rule_buf),
                     "-d ::1 -p tcp -m tcp --dport %d -j REJECT",
                     cfg->tcp_port);
            tproxy_rule_del(cfg, 6, "filter", "OUTPUT", rule_buf);
        }
    }
    
    return 0;
}

int tproxy_xfrm_bypass(atp_config_t *cfg) {
    LOG_INFO("Setting up XFRM bypass chain");
    
    if (!tproxy_chain_exists(cfg, 4, "mangle", "XFRM_BYPASS")) {
        tproxy_chain_create(cfg, 4, "mangle", "XFRM_BYPASS");
    }
    tproxy_chain_flush(cfg, 4, "mangle", "XFRM_BYPASS");
    
    tproxy_rule_add(cfg, 4, "mangle", "XFRM_BYPASS", "-p esp -j RETURN");
    tproxy_rule_add(cfg, 4, "mangle", "XFRM_BYPASS", "-p udp --dport 4500 -j RETURN");
    tproxy_rule_add(cfg, 4, "mangle", "XFRM_BYPASS", "-p udp --dport 500 -j RETURN");
    
    tproxy_rule_del(cfg, 4, "mangle", "PREROUTING", "-j XFRM_BYPASS");
    tproxy_rule_insert(cfg, 4, "mangle", "PREROUTING", 1, "-j XFRM_BYPASS");
    
    return 0;
}

int tproxy_prevent_loop(atp_config_t *cfg) {
    char rule_buf[64];
    snprintf(rule_buf, sizeof(rule_buf), "-m mark --mark %d -j RETURN", cfg->mark_value);
    
    tproxy_rule_del(cfg, 4, "mangle", "PREROUTING", rule_buf);
    tproxy_rule_insert(cfg, 4, "mangle", "PREROUTING", 1, rule_buf);
    
    return 0;
}