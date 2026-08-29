#ifndef ATP_EBPF_H
#define ATP_EBPF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "atp.h"

typedef struct {
    bool supported;
    bool has_lpm_trie;
    bool has_array;
    bool has_hash;
    bool has_lru_hash;
    bool has_cgroup_sock_addr;
    bool has_sched_cls;
    char kernel_release[64];
} ebpf_probe_result_t;

typedef struct {
    uint64_t active_conns;
} atp_ebpf_telemetry_t;

atp_result_t ebpf_probe(void);
atp_result_t ebpf_probe_detailed(ebpf_probe_result_t *res);
atp_result_t ebpf_status(char *state, size_t size, atp_config_t *cfg);
bool ebpf_is_pure_mode(const atp_config_t *cfg);
int ebpf_get_telemetry(atp_ebpf_telemetry_t *stats);

#endif
