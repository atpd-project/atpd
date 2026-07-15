#ifndef ATP_TPROXY_H
#define ATP_TPROXY_H

#include "atp.h"

#define CHAIN_PRE_IPV4          "ATP_PRE_0"
#define CHAIN_PRE_IPV4_1        "ATP_PRE_1"
#define CHAIN_OUT_IPV4          "ATP_OUT_0"
#define CHAIN_OUT_IPV4_1        "ATP_OUT_1"
#define CHAIN_DIVERT_IPV4       "ATP_DIVERT_0"
#define CHAIN_DIVERT_IPV4_1     "ATP_DIVERT_1"
#define CHAIN_PROXY_IP_IPV4     "ATP_PROXY_IP_0"
#define CHAIN_PROXY_IP_IPV4_1   "ATP_PROXY_IP_1"
#define CHAIN_BYPASS_IP_IPV4    "ATP_BYPASS_IP_0"
#define CHAIN_BYPASS_IP_IPV4_1  "ATP_BYPASS_IP_1"
#define CHAIN_PROXY_IFACE_IPV4  "ATP_PROXY_IFACE_0"
#define CHAIN_PROXY_IFACE_IPV4_1 "ATP_PROXY_IFACE_1"
#define CHAIN_BYPASS_IFACE_IPV4 "ATP_BYPASS_IFACE_0"
#define CHAIN_BYPASS_IFACE_IPV4_1 "ATP_BYPASS_IFACE_1"
#define CHAIN_DNS_PRE_IPV4      "ATP_DNS_PRE_0"
#define CHAIN_DNS_PRE_IPV4_1    "ATP_DNS_PRE_1"
#define CHAIN_DNS_OUT_IPV4      "ATP_DNS_OUT_0"
#define CHAIN_DNS_OUT_IPV4_1    "ATP_DNS_OUT_1"
#define CHAIN_APP_IPV4          "ATP_APP_0"
#define CHAIN_APP_IPV4_1        "ATP_APP_1"
#define CHAIN_MAC_IPV4          "ATP_MAC_0"
#define CHAIN_MAC_IPV4_1        "ATP_MAC_1"
#define CHAIN_REDIRECT_IPV4     "ATP_REDIRECT"
#define CHAIN_REDIRECT_TCP_IPV4 "ATP_REDIRECT_TCP"
#define CHAIN_UDP_TPROXY_IPV4   "ATP_UDP_TPROXY"
#define CHAIN_QUIC_IPV4         "ATP_QUIC_0"
#define CHAIN_XFRM_BYPASS_IPV4  "XFRM_BYPASS"
#define CHAIN_XFRM_BYPASS_NAT_IPV4 "XFRM_BYPASS_NAT"

#define CHAIN_PRE_IPV6          "ATP6_PRE_0"
#define CHAIN_PRE_IPV6_1        "ATP6_PRE_1"
#define CHAIN_OUT_IPV6          "ATP6_OUT_0"
#define CHAIN_OUT_IPV6_1        "ATP6_OUT_1"
#define CHAIN_DIVERT_IPV6       "ATP6_DIVERT_0"
#define CHAIN_DIVERT_IPV6_1     "ATP6_DIVERT_1"
#define CHAIN_PROXY_IP_IPV6     "ATP6_PROXY_IP_0"
#define CHAIN_PROXY_IP_IPV6_1   "ATP6_PROXY_IP_1"
#define CHAIN_BYPASS_IP_IPV6    "ATP6_BYPASS_IP_0"
#define CHAIN_BYPASS_IP_IPV6_1  "ATP6_BYPASS_IP_1"
#define CHAIN_PROXY_IFACE_IPV6  "ATP6_PROXY_IFACE_0"
#define CHAIN_PROXY_IFACE_IPV6_1 "ATP6_PROXY_IFACE_1"
#define CHAIN_BYPASS_IFACE_IPV6 "ATP6_BYPASS_IFACE_0"
#define CHAIN_BYPASS_IFACE_IPV6_1 "ATP6_BYPASS_IFACE_1"
#define CHAIN_DNS_PRE_IPV6      "ATP6_DNS_PRE_0"
#define CHAIN_DNS_PRE_IPV6_1    "ATP6_DNS_PRE_1"
#define CHAIN_DNS_OUT_IPV6      "ATP6_DNS_OUT_0"
#define CHAIN_DNS_OUT_IPV6_1    "ATP6_DNS_OUT_1"
#define CHAIN_APP_IPV6          "ATP6_APP_0"
#define CHAIN_APP_IPV6_1        "ATP6_APP_1"
#define CHAIN_MAC_IPV6          "ATP6_MAC_0"
#define CHAIN_MAC_IPV6_1        "ATP6_MAC_1"
#define CHAIN_REDIRECT_IPV6     "ATP6_REDIRECT"
#define CHAIN_REDIRECT_TCP_IPV6 "ATP6_REDIRECT_TCP"
#define CHAIN_UDP_TPROXY_IPV6   "ATP6_UDP_TPROXY"
#define CHAIN_QUIC_IPV6         "ATP6_QUIC_0"

int tproxy_support_check(atp_config_t *cfg);
int tproxy_setup_ipv4(atp_config_t *cfg);
int tproxy_setup_ipv6(atp_config_t *cfg);
int tproxy_setup_redirect_ipv4(atp_config_t *cfg);
int tproxy_setup_redirect_ipv6(atp_config_t *cfg);
int tproxy_setup_enhance_ipv4(atp_config_t *cfg);
int tproxy_setup_enhance_ipv6(atp_config_t *cfg);

int tproxy_cleanup_ipv4(atp_config_t *cfg);
int tproxy_cleanup_ipv6(atp_config_t *cfg);
int tproxy_cleanup_redirect_ipv4(atp_config_t *cfg);
int tproxy_cleanup_redirect_ipv6(atp_config_t *cfg);
int tproxy_cleanup_enhance_ipv4(atp_config_t *cfg);
int tproxy_cleanup_enhance_ipv6(atp_config_t *cfg);
int tproxy_cleanup_xfrm_bypass(atp_config_t *cfg);
int tproxy_cleanup_all(atp_config_t *cfg);
int tproxy_cleanup_orphan_chains(atp_config_t *cfg);
int tproxy_remove_all_hooks(atp_config_t *cfg);

int tproxy_rule_exists(atp_config_t *cfg, int family, const char *table,
                       const char *chain, const char *rule);
int tproxy_rule_ensure_single(atp_config_t *cfg, int family, const char *table,
                              const char *chain, const char *rule);
int tproxy_rule_ensure_single_insert(atp_config_t *cfg, int family, const char *table,
                                     const char *chain, int position, const char *rule);
int tproxy_restart(atp_config_t *cfg);

int tproxy_dns_hijack_setup(atp_config_t *cfg, int family, int mode);
int tproxy_dns_hijack_cleanup(atp_config_t *cfg, int family);

int tproxy_block_quic(atp_config_t *cfg, int enable);
int tproxy_block_loopback(atp_config_t *cfg, int enable);
int tproxy_xfrm_bypass(atp_config_t *cfg);
int tproxy_prevent_loop(atp_config_t *cfg);

int tproxy_chain_create(atp_config_t *cfg, int family, const char *table, const char *chain);
int tproxy_chain_flush(atp_config_t *cfg, int family, const char *table, const char *chain);
int tproxy_chain_destroy(atp_config_t *cfg, int family, const char *table, const char *chain);
int tproxy_chain_exists(atp_config_t *cfg, int family, const char *table, const char *chain);

int tproxy_rule_add(atp_config_t *cfg, int family, const char *table,
                    const char *chain, const char *rule);
int tproxy_rule_del(atp_config_t *cfg, int family, const char *table,
                    const char *chain, const char *rule);
int tproxy_rule_insert(atp_config_t *cfg, int family, const char *table,
                       const char *chain, int position, const char *rule);

void tproxy_hook_main_chains(atp_config_t *cfg, int family);

#define SAFE_SNPRINTF(buf, size, ...) do { \
    int _n = snprintf(buf, size, __VA_ARGS__); \
    if (_n < 0 || _n >= (int)size) { \
        LOG_ERROR("snprintf truncation in %s:%d", __FILE__, __LINE__); \
    } \
} while(0)

#endif
