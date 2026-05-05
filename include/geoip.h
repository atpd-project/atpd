#ifndef ATP_GEOIP_H
#define ATP_GEOIP_H

#include "atp.h"

int geoip_init(atp_config_t *cfg);
int geoip_download(atp_config_t *cfg);
int geoip_setup_ipset(atp_config_t *cfg);
int geoip_setup_ipset_async(atp_config_t *cfg);
int geoip_cleanup_ipset(atp_config_t *cfg);
int geoip_atomic_update(atp_config_t *cfg);
int geoip_check_update_needed(atp_config_t *cfg, int max_age_days);
int geoip_force_update(atp_config_t *cfg);
int geoip_async_is_complete(void);

int geoip_validate_cidr(const char *cidr, int family);

#endif
