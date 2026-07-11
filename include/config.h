#ifndef CONFIG_H
#define CONFIG_H

#include "atp.h"

void config_set_defaults(atp_config_t *cfg);
int config_load(const char *path, atp_config_t *cfg);
int config_save_runtime(const char *path, atp_config_t *cfg);
int config_set_mode(atp_config_t *cfg, const char *mode);
int config_reload(atp_config_t *cfg);

int validate_interface_name(const char *name);
int validate_port(int port);
int validate_mark(int mark);

#endif
