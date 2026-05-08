#ifndef ATP_ROUTING_H
#define ATP_ROUTING_H

#include "atp.h"

int routing_rule_add(atp_config_t *cfg, int family, const char *rule);
int routing_rule_del(atp_config_t *cfg, int family, const char *rule);
int routing_rule_del_by_pref(atp_config_t *cfg, int family, int pref);

int routing_route_add(atp_config_t *cfg, int family, const char *route);
int routing_route_del(atp_config_t *cfg, int family, const char *route);

int routing_setup_ipv4(atp_config_t *cfg);
int routing_setup_ipv6(atp_config_t *cfg);
int routing_cleanup_ipv4(atp_config_t *cfg);
int routing_cleanup_ipv6(atp_config_t *cfg);
int routing_cleanup_all(atp_config_t *cfg);

int routing_ip_forward_enable(atp_config_t *cfg, int enable);
int routing_ipv6_forward_enable(atp_config_t *cfg, int enable);

#endif
