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

/* ipset operations - now wrappers around ipset.c */
int geoip_ipset_create(const char *name, int family, int hashsize, int maxelem);
int geoip_ipset_destroy(const char *name);
int geoip_ipset_swap(const char *from, const char *to);
int geoip_ipset_exists(const char *name);
int geoip_ipset_flush(const char *name);
int geoip_ipset_restore_file(const char *name, const char *filename);
int geoip_parse_cidr_file(const char *input_path, const char *output_path, int family);
int geoip_validate_cidr(const char *cidr, int family);

#endif
