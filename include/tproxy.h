#ifndef ATP_TPROXY_H
#define ATP_TPROXY_H

#include "atp.h"

int tproxy_support_check(atp_config_t *cfg);
int tproxy_setup_ipv4(atp_config_t *cfg);
int tproxy_setup_ipv6(atp_config_t *cfg);
int tproxy_setup_redirect_ipv4(atp_config_t *cfg);
int tproxy_setup_redirect_ipv6(atp_config_t *cfg);
int tproxy_setup_enhance_ipv4(atp_config_t *cfg);
int tproxy_setup_enhance_ipv6(atp_config_t *cfg);
int tproxy_cleanup_ipv4(atp_config_t *cfg);
int tproxy_cleanup_ipv6(atp_config_t *cfg);
int tproxy_cleanup_all(atp_config_t *cfg);
int tproxy_cleanup_xfrm_bypass(atp_config_t *cfg);

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

int tproxy_atomic_switch(atp_config_t *cfg, int family, const char *table,
                         const char *hook, const char *chain0, const char *chain1);

int tproxy_setup_ipv4_batch(atp_config_t *cfg);
int tproxy_setup_ipv6_batch(atp_config_t *cfg);

int tproxy_refresh_rules(atp_config_t *cfg);
int ip_rule_audit(atp_config_t *cfg);

#endif
