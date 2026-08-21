#ifndef ATP_CONFIG_VALIDATOR_H
#define ATP_CONFIG_VALIDATOR_H

#include "atp_config.h"

int config_validate_key(const char *key, atp_config_t *cfg);
int config_validate_values(atp_config_t *cfg);
void config_set_strict_mode(int strict);
int config_get_strict_mode(void);

#endif
