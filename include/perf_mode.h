#ifndef ATP_PERF_MODE_H
#define ATP_PERF_MODE_H

#include "atp.h"

typedef struct {
    int conntrack_enabled;
    int socket_match_enabled;
    int divert_enabled;
    int connmark_enabled;
} perf_features_t;

int perf_mode_init(atp_config_t *cfg);
int perf_mode_setup(atp_config_t *cfg);
int perf_mode_cleanup(atp_config_t *cfg);
int perf_mode_tune_tcp_stack(atp_config_t *cfg);
int perf_mode_enable_conntrack_optimization(atp_config_t *cfg);
int perf_mode_enable_socket_match(atp_config_t *cfg);

#endif
