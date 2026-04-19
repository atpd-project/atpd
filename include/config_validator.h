/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2025 ATP Project
 *
 * Configuration validator - strict mode with helpful suggestions
 * TODO: Full implementation
 */

#ifndef ATP_CONFIG_VALIDATOR_H
#define ATP_CONFIG_VALIDATOR_H

#include "atp.h"

static inline int config_validate_key(const char *key, atp_config_t *cfg) {
    (void)key;
    (void)cfg;
    return 0;
}

static inline int config_validate_values(atp_config_t *cfg) {
    (void)cfg;
    return 0;
}

static inline void config_set_strict_mode(int strict) {
    (void)strict;
}

static inline int config_get_strict_mode(void) {
    return 0;
}

#endif /* ATP_CONFIG_VALIDATOR_H */
