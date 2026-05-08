#include "tproxy.h"
#include "logger.h"
#include "utils.h"
#include "config.h"
#include "atp.h"
#include "nft.h"
#include <stdlib.h>
#include <string.h>

static nft_ctx_t *g_nft_ctx = NULL;

static int nft_setup_begin(int family) {
    if (g_nft_ctx) nft_destroy(g_nft_ctx);
    g_nft_ctx = nft_create(family);
    if (!g_nft_ctx) return -1;
    return nft_begin(g_nft_ctx);
}

static int nft_setup_commit(void) {
    int ret = nft_commit(g_nft_ctx);
    nft_destroy(g_nft_ctx);
    g_nft_ctx = NULL;
    return ret;
}

static int nft_cleanup_flush(int family) {
    nft_ctx_t *ctx = nft_create(family);
    if (!ctx) return -1;
    nft_flush_table(ctx, "atp");
    nft_destroy(ctx);
    return 0;
}

static int exec_nft_rule(atp_config_t *cfg, const char *table, const char *chain, const char *rule) {
    (void)cfg;
    if (!g_nft_ctx || !rule) return 0;
    return nft_add_rule(g_nft_ctx, table, chain, rule);
}

int tproxy_chain_exists(atp_config_t *cfg, int family, const char *table, const char *chain) {
    (void)cfg; (void)family; (void)table; (void)chain;
    return 0;
}

int tproxy_chain_create(atp_config_t *cfg, int family, const char *table, const char *chain) {
    (void)cfg; (void)family; (void)table; (void)chain;
    return 0;
}

int tproxy_chain_flush(atp_config_t *cfg, int family, const char *table, const char *chain) {
    (void)cfg; (void)family; (void)table; (void)chain;
    return 0;
}

int tproxy_chain_destroy(atp_config_t *cfg, int family, const char *table, const char *chain) {
    (void)cfg; (void)family; (void)table; (void)chain;
    return 0;
}

int tproxy_rule_add(atp_config_t *cfg, int family, const char *table,
                    const char *chain, const char *rule) {
    return exec_nft_rule(cfg, table, chain, rule);
}

int tproxy_rule_del(atp_config_t *cfg, int family, const char *table,
                    const char *chain, const char *rule) {
    (void)cfg; (void)family; (void)table; (void)chain; (void)rule;
    return 0;
}

int tproxy_rule_insert(atp_config_t *cfg, int family, const char *table,
                       const char *chain, int position, const char *rule) {
    return exec_nft_rule(cfg, table, chain, rule);
}

int tproxy_atomic_switch(atp_config_t *cfg, int family, const char *table,
                         const char *hook, const char *chain0, const char *chain1) {
    (void)cfg; (void)family; (void)table; (void)hook; (void)chain0; (void)chain1;
    return 0;
}

int tproxy_support_check(atp_config_t *cfg) {
    (void)cfg;
    return 1;
}

static void tproxy_setup_divert_chain(atp_config_t *cfg, int family, const char *suffix, int mark) {
    char rule[256];
    snprintf(rule, sizeof(rule), "-j MARK --set-mark %d", mark);
    exec_nft_rule(cfg, "atp", "ATP_DIVERT", rule);
    exec_nft_rule(cfg, "atp", "ATP_DIVERT", "-j ACCEPT");
}

static void tproxy_setup_socket_match(atp_config_t *cfg, int family, const char *suffix, const char *divert_chain) {
    char rule[256];
    snprintf(rule, sizeof(rule), "-p tcp -m socket --transparent -j %s", divert_chain);
    exec_nft_rule(cfg, "atp", "ATP_PRE", rule);
}

static void tproxy_setup_chain_jumps(atp_config_t *cfg, int family, const char *suffix) {
    (void)suffix;

    exec_nft_rule(cfg, "atp", "ATP_PRE", "-j ATP_PROXY_IP");
    exec_nft_rule(cfg, "atp", "ATP_PRE", "-j ATP_BYPASS_IP");
    exec_nft_rule(cfg, "atp", "ATP_PRE", "-j ATP_PROXY_IFACE");
    exec_nft_rule(cfg, "atp", "ATP_PRE", "-j ATP_MAC");
    exec_nft_rule(cfg, "atp", "ATP_PRE", "-j ATP_DNS_PRE");

    exec_nft_rule(cfg, "atp", "ATP_OUT", "-j ATP_PROXY_IP");
    exec_nft_rule(cfg, "atp", "ATP_OUT", "-j ATP_BYPASS_IP");
    exec_nft_rule(cfg, "atp", "ATP_OUT", "-j ATP_BYPASS_IFACE");
    exec_nft_rule(cfg, "atp", "ATP_OUT", "-j ATP_MAC");
    exec_nft_rule(cfg, "atp", "ATP_OUT", "-j ATP_DNS_OUT");
}

static void tproxy_setup_iface_rules(atp_config_t *cfg, int family, const char *suffix) {
    char rule[256];
    (void)suffix;

    exec_nft_rule(cfg, "atp", "ATP_PROXY_IFACE", "-i lo -j RETURN");

    if (cfg->proxy_mobile) {
        snprintf(rule, sizeof(rule), "-i %s -j RETURN", cfg->mobile_iface);
        exec_nft_rule(cfg, "atp", "ATP_PROXY_IFACE", rule);
    }

    int hotspot_on_wifi = (strcmp(cfg->hotspot_iface, cfg->wifi_iface) == 0);

    if (hotspot_on_wifi) {
        if (cfg->proxy_hotspot) {
            snprintf(rule, sizeof(rule), "-i %s -s %s -j RETURN",
                     cfg->hotspot_iface, cfg->hotspot_subnet_ipv4);
            exec_nft_rule(cfg, "atp", "ATP_PROXY_IFACE", rule);
        } else {
            snprintf(rule, sizeof(rule), "-i %s -s %s -j ACCEPT",
                     cfg->hotspot_iface, cfg->hotspot_subnet_ipv4);
            exec_nft_rule(cfg, "atp", "ATP_PROXY_IFACE", rule);
        }

        if (cfg->proxy_wifi) {
            snprintf(rule, sizeof(rule), "-i %s ! -s %s -j RETURN",
                     cfg->wifi_iface, cfg->hotspot_subnet_ipv4);
            exec_nft_rule(cfg, "atp", "ATP_PROXY_IFACE", rule);
        } else {
            snprintf(rule, sizeof(rule), "-i %s ! -s %s -j ACCEPT",
                     cfg->wifi_iface, cfg->hotspot_subnet_ipv4);
            exec_nft_rule(cfg, "atp", "ATP_PROXY_IFACE", rule);
            snprintf(rule, sizeof(rule), "-o %s -j ACCEPT", cfg->wifi_iface);
            exec_nft_rule(cfg, "atp", "ATP_BYPASS_IFACE", rule);
        }
    } else {
        if (cfg->proxy_wifi) {
            snprintf(rule, sizeof(rule), "-i %s -j RETURN", cfg->wifi_iface);
            exec_nft_rule(cfg, "atp", "ATP_PROXY_IFACE", rule);
        } else {
            snprintf(rule, sizeof(rule), "-i %s -j ACCEPT", cfg->wifi_iface);
            exec_nft_rule(cfg, "atp", "ATP_PROXY_IFACE", rule);
            snprintf(rule, sizeof(rule), "-o %s -j ACCEPT", cfg->wifi_iface);
            exec_nft_rule(cfg, "atp", "ATP_BYPASS_IFACE", rule);
        }

        if (cfg->proxy_hotspot) {
            snprintf(rule, sizeof(rule), "-i %s -j RETURN", cfg->hotspot_iface);
            exec_nft_rule(cfg, "atp", "ATP_PROXY_IFACE", rule);
        } else {
            snprintf(rule, sizeof(rule), "-i %s -j ACCEPT", cfg->hotspot_iface);
            exec_nft_rule(cfg, "atp", "ATP_PROXY_IFACE", rule);
            snprintf(rule, sizeof(rule), "-o %s -j ACCEPT", cfg->hotspot_iface);
            exec_nft_rule(cfg, "atp", "ATP_BYPASS_IFACE", rule);
        }
    }

    if (cfg->proxy_usb) {
        snprintf(rule, sizeof(rule), "-i %s -j RETURN", cfg->usb_iface);
        exec_nft_rule(cfg, "atp", "ATP_PROXY_IFACE", rule);
    } else {
        snprintf(rule, sizeof(rule), "-i %s -j ACCEPT", cfg->usb_iface);
        exec_nft_rule(cfg, "atp", "ATP_PROXY_IFACE", rule);
    }

    if (cfg->other_proxy[0]) {
        char *list = strdup(cfg->other_proxy);
        if (list) {
            char *token = strtok(list, " ");
            while (token) {
                snprintf(rule, sizeof(rule), "-i %s -j RETURN", token);
                exec_nft_rule(cfg, "atp", "ATP_PROXY_IFACE", rule);
                token = strtok(NULL, " ");
            }
            free(list);
        }
    }

    if (cfg->other_bypass[0]) {
        char *list = strdup(cfg->other_bypass);
        if (list) {
            char *token = strtok(list, " ");
            while (token) {
                snprintf(rule, sizeof(rule), "-i %s -j ACCEPT", token);
                exec_nft_rule(cfg, "atp", "ATP_PROXY_IFACE", rule);
                snprintf(rule, sizeof(rule), "-o %s -j ACCEPT", token);
                exec_nft_rule(cfg, "atp", "ATP_BYPASS_IFACE", rule);
                token = strtok(NULL, " ");
            }
            free(list);
        }
    }
}

static void tproxy_setup_ip_rules(atp_config_t *cfg, int family, const char *suffix) {
    char rule[256];
    (void)suffix;

    if (cfg->proxy_ipv4_list[0]) {
        char *list = strdup(cfg->proxy_ipv4_list);
        if (list) {
            char *token = strtok(list, " ");
            while (token) {
                snprintf(rule, sizeof(rule), "-d %s -j RETURN", token);
                exec_nft_rule(cfg, "atp", "ATP_PROXY_IP", rule);
                token = strtok(NULL, " ");
            }
            free(list);
        }
    }

    if (cfg->bypass_ipv4_list[0]) {
        char *list = strdup(cfg->bypass_ipv4_list);
        if (list) {
            char *token = strtok(list, " ");
            while (token) {
                snprintf(rule, sizeof(rule), "-d %s -p udp ! --dport 53 -j ACCEPT", token);
                exec_nft_rule(cfg, "atp", "ATP_BYPASS_IP", rule);
                snprintf(rule, sizeof(rule), "-d %s ! -p udp -j ACCEPT", token);
                exec_nft_rule(cfg, "atp", "ATP_BYPASS_IP", rule);
                token = strtok(NULL, " ");
            }
            free(list);
        }
    }

    if (cfg->bypass_cn_ip) {
        snprintf(rule, sizeof(rule), "-m set --match-set cnip dst -j ACCEPT");
        exec_nft_rule(cfg, "atp", "ATP_BYPASS_IP", rule);
    }
}

void tproxy_hook_main_chains(atp_config_t *cfg, int family, const char *suffix) {
    char hook_rule[128];
    (void)suffix;

    snprintf(hook_rule, sizeof(hook_rule),
             "-m owner --uid-owner %s --gid-owner %s -j RETURN",
             cfg->core_user, cfg->core_group);
    exec_nft_rule(cfg, "atp", "OUTPUT", hook_rule);

    exec_nft_rule(cfg, "atp", "PREROUTING", "-j ATP_PRE");
    exec_nft_rule(cfg, "atp", "OUTPUT", "-j ATP_OUT");
}

int tproxy_setup_ipv4(atp_config_t *cfg) {
    LOG_INFO("Setting up IPv4 nftables chains");

    if (nft_setup_begin(NFPROTO_IPV4) < 0) return -1;

    nft_add_chain(g_nft_ctx, "atp", "PREROUTING", "filter", -150);
    nft_add_chain(g_nft_ctx, "atp", "OUTPUT", "filter", -150);

    tproxy_setup_divert_chain(cfg, 4, "", cfg->mark_value);
    tproxy_setup_socket_match(cfg, 4, "", "ATP_DIVERT");
    tproxy_setup_chain_jumps(cfg, 4, "");
    tproxy_setup_iface_rules(cfg, 4, "");
    tproxy_setup_ip_rules(cfg, 4, "");
    tproxy_hook_main_chains(cfg, 4, "");

    int ret = nft_setup_commit();
    LOG_INFO("IPv4 nftables setup complete (ret=%d)", ret);
    return ret;
}

int tproxy_setup_ipv6(atp_config_t *cfg) {
    if (!cfg->proxy_ipv6) {
        LOG_DEBUG("IPv6 proxy disabled, skipping");
        return 0;
    }

    LOG_INFO("Setting up IPv6 nftables chains");

    if (nft_setup_begin(NFPROTO_IPV6) < 0) return -1;

    nft_add_chain(g_nft_ctx, "atp", "PREROUTING", "filter", -150);
    nft_add_chain(g_nft_ctx, "atp", "OUTPUT", "filter", -150);

    tproxy_setup_divert_chain(cfg, 6, "6", cfg->mark_value6);
    tproxy_setup_socket_match(cfg, 6, "6", "ATP_DIVERT");
    tproxy_setup_chain_jumps(cfg, 6, "6");
    tproxy_setup_iface_rules(cfg, 6, "6");
    tproxy_setup_ip_rules(cfg, 6, "6");
    tproxy_hook_main_chains(cfg, 6, "6");

    int ret = nft_setup_commit();
    LOG_INFO("IPv6 nftables setup complete (ret=%d)", ret);
    return ret;
}

int tproxy_setup_redirect_ipv4(atp_config_t *cfg) {
    LOG_INFO("Setting up IPv4 REDIRECT chains");

    if (nft_setup_begin(NFPROTO_IPV4) < 0) return -1;

    nft_add_chain(g_nft_ctx, "atp", "PREROUTING", "nat", -100);
    nft_add_chain(g_nft_ctx, "atp", "OUTPUT", "nat", -100);

    char rule[256];
    snprintf(rule, sizeof(rule), "-p tcp -j REDIRECT --to-ports %d", cfg->tcp_port);
    exec_nft_rule(cfg, "atp", "ATP_REDIRECT", rule);

    exec_nft_rule(cfg, "atp", "PREROUTING", "-j ATP_REDIRECT");
    exec_nft_rule(cfg, "atp", "OUTPUT", "-j ATP_REDIRECT");

    int ret = nft_setup_commit();
    LOG_INFO("IPv4 REDIRECT setup complete (ret=%d)", ret);
    return ret;
}

int tproxy_setup_enhance_ipv4(atp_config_t *cfg) {
    LOG_INFO("Setting up ENHANCE mode for IPv4 (TCP=REDIRECT:%d, UDP=TPROXY:%d)",
             cfg->redirect_tcp_port, cfg->udp_port);

    if (nft_setup_begin(NFPROTO_IPV4) < 0) return -1;

    char rule[256];

    nft_add_chain(g_nft_ctx, "atp-nat", "PREROUTING", "nat", -100);
    nft_add_chain(g_nft_ctx, "atp-nat", "OUTPUT", "nat", -100);

    snprintf(rule, sizeof(rule), "-p tcp -j REDIRECT --to-ports %d", cfg->redirect_tcp_port);
    exec_nft_rule(cfg, "atp-nat", "ATP_REDIRECT_TCP", rule);
    exec_nft_rule(cfg, "atp-nat", "PREROUTING", "-j ATP_REDIRECT_TCP");
    exec_nft_rule(cfg, "atp-nat", "OUTPUT", "-j ATP_REDIRECT_TCP");

    nft_add_chain(g_nft_ctx, "atp", "PREROUTING", "filter", -150);
    nft_add_chain(g_nft_ctx, "atp", "OUTPUT", "filter", -150);

    snprintf(rule, sizeof(rule), "-p udp -j TPROXY --on-port %d --tproxy-mark %d",
             cfg->udp_port, cfg->mark_value);
    exec_nft_rule(cfg, "atp", "ATP_UDP_TPROXY", rule);
    exec_nft_rule(cfg, "atp", "PREROUTING", "-j ATP_UDP_TPROXY");

    snprintf(rule, sizeof(rule), "-p udp -j MARK --set-mark %d", cfg->mark_value);
    exec_nft_rule(cfg, "atp", "OUTPUT", rule);

    snprintf(rule, sizeof(rule), "-p tcp -m owner --uid-owner %s --gid-owner %s -j ACCEPT",
             cfg->core_user, cfg->core_group);
    exec_nft_rule(cfg, "atp-nat", "OUTPUT", rule);

    int ret = nft_setup_commit();
    LOG_INFO("IPv4 ENHANCE mode setup complete (ret=%d)", ret);
    return ret;
}

int tproxy_cleanup_ipv4(atp_config_t *cfg) {
    (void)cfg;
    LOG_INFO("Cleaning up IPv4 nftables chains");
    return nft_cleanup_flush(NFPROTO_IPV4);
}

int tproxy_cleanup_ipv6(atp_config_t *cfg) {
    (void)cfg;
    LOG_INFO("Cleaning up IPv6 nftables chains");
    return nft_cleanup_flush(NFPROTO_IPV6);
}

int tproxy_cleanup_all(atp_config_t *cfg) {
    tproxy_cleanup_ipv4(cfg);
    if (cfg->proxy_ipv6) tproxy_cleanup_ipv6(cfg);
    return 0;
}

int tproxy_dns_hijack_setup(atp_config_t *cfg, int family, int mode) {
    if (cfg->dns_hijack == DNS_HIJACK_OFF) return 0;
    if (mode == DNS_HIJACK_OFF) return 0;

    char rule[128];

    if (mode == DNS_HIJACK_TPROXY) {
        snprintf(rule, sizeof(rule),
                 "-p udp --dport 53 -j TPROXY --on-port %d --tproxy-mark %d",
                 cfg->dns_port, cfg->mark_value);
    } else if (mode == DNS_HIJACK_REDIRECT) {
        snprintf(rule, sizeof(rule),
                 "-p udp --dport 53 -j REDIRECT --to-ports %d",
                 cfg->dns_port);
    } else {
        return 0;
    }

    exec_nft_rule(cfg, "atp", "ATP_DNS_PRE", rule);
    exec_nft_rule(cfg, "atp", "ATP_DNS_OUT", rule);
    return 0;
}

int tproxy_dns_hijack_cleanup(atp_config_t *cfg, int family) {
    (void)cfg; (void)family;
    return 0;
}

int tproxy_block_quic(atp_config_t *cfg, int enable) {
    if (enable) {
        LOG_INFO("Enabling QUIC blocking");
        exec_nft_rule(cfg, "atp", "ATP_QUIC", "-p udp --dport 443 -j DROP");
        exec_nft_rule(cfg, "atp", "INPUT", "-j ATP_QUIC");
        exec_nft_rule(cfg, "atp", "FORWARD", "-j ATP_QUIC");
        exec_nft_rule(cfg, "atp", "OUTPUT", "-j ATP_QUIC");
    } else {
        LOG_INFO("Disabling QUIC blocking");
    }
    return 0;
}

int tproxy_xfrm_bypass(atp_config_t *cfg) {
    (void)cfg;
    return 0;
}

int tproxy_cleanup_xfrm_bypass(atp_config_t *cfg) {
    (void)cfg;
    return 0;
}

int tproxy_block_loopback(atp_config_t *cfg, int enable) {
    (void)cfg; (void)enable;
    return 0;
}

int tproxy_prevent_loop(atp_config_t *cfg) {
    (void)cfg;
    return 0;
}
