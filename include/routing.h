#ifndef ATP_ROUTING_H
#define ATP_ROUTING_H

#include "atp.h"

int routing_setup_ipv4(atp_config_t *cfg);
int routing_setup_ipv6(atp_config_t *cfg);
int routing_cleanup_ipv4(atp_config_t *cfg);
int routing_cleanup_ipv6(atp_config_t *cfg);
int routing_cleanup_all(atp_config_t *cfg);

int routing_rule_add(atp_config_t *cfg, int family, const char *rule);
int routing_rule_del(atp_config_t *cfg, int family, const char *rule);
int routing_rule_del_by_pref(atp_config_t *cfg, int family, int pref);
int routing_rule_del_all_by_pref(atp_config_t *cfg, int family, int pref);

int routing_route_add(atp_config_t *cfg, int family, const char *route);
int routing_route_del(atp_config_t *cfg, int family, const char *route);
int routing_route_flush_table(atp_config_t *cfg, int family, int table_id);

int routing_add_vpn_policy(atp_config_t *cfg, const char *vpn_iface);
int routing_remove_vpn_policy(atp_config_t *cfg, const char *vpn_iface);
int routing_add_mss_clamp(atp_config_t *cfg, const char *iface);
int routing_remove_mss_clamp(atp_config_t *cfg, const char *iface);

int routing_ip_forward_enable(atp_config_t *cfg, int enable);
int routing_ipv6_forward_enable(atp_config_t *cfg, int enable);

int routing_rp_filter_set(atp_config_t *cfg, int value);
int routing_tcp_stack_tune(atp_config_t *cfg);

int routing_get_active_interfaces(char *ifaces, size_t size);
int routing_get_ipv4_addrs(const char *iface, char *output, size_t size);

#endif