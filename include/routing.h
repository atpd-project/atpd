#ifndef ATP_ROUTING_H
#define ATP_ROUTING_H

#include "atp_config.h"

int routing_add_vpn_policy(atp_config_t *cfg, const char *vpn_iface);
int routing_remove_vpn_policy(atp_config_t *cfg, const char *vpn_iface);

#endif
