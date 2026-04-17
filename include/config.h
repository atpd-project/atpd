#ifndef ATP_CONFIG_H
#define ATP_CONFIG_H

#include "atp.h"

void config_set_defaults(atp_config_t *cfg);
int config_load(const char *path, atp_config_t *cfg);
int config_save(const char *path, atp_config_t *cfg);
int config_save_runtime(const char *path, atp_config_t *cfg);
int config_load_runtime(const char *path, atp_config_t *cfg);
int config_set_mode(atp_config_t *cfg, const char *mode);
int config_reload(atp_config_t *cfg);
void config_print_summary(atp_config_t *cfg);

int validate_user_group(const char *user, const char *group);
int validate_interface_name(const char *name);
int validate_ip_cidr(const char *cidr);
int validate_port(int port);
int validate_mark(int mark);

#endif