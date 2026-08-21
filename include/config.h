#ifndef ATP_CONFIG_H
#define ATP_CONFIG_H

#include "atp_config.h"

void config_set_defaults(atp_config_t *cfg);
int config_load(const char *path, atp_config_t *cfg);
int config_reload(atp_config_t *cfg);

#endif
