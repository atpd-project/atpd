/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2025 ATP Project
 *
 * Configuration validator header
 */

#ifndef ATP_CONFIG_VALIDATOR_H
#define ATP_CONFIG_VALIDATOR_H

#include "atp.h"

typedef enum {
    CONFIG_VALUE_BOOL,
    CONFIG_VALUE_INT,
    CONFIG_VALUE_STRING
} config_value_type_t;

typedef struct {
    const char *name;
    config_value_type_t type;
    bool deprecated;
    const char *canonical_name;
    bool allow_empty;
} config_key_spec_t;

const config_key_spec_t *config_schema_find(const char *key);
int config_validate_key(const char *key);
int config_validate_values(const atp_config_t *cfg);

#endif /* ATP_CONFIG_VALIDATOR_H */
