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
    uint64_t total_packets;
    uint64_t total_bytes;
    uint64_t active_conns;
    uint64_t dns_intercepts;
    bool map_direct;
} atp_ebpf_telemetry_t;

typedef struct {
    bool is_active;
    uint32_t flags;
    uint32_t self_tgid;
    uint16_t listener_port;
    uint32_t active_flows;
    uint32_t uid_rules_count;
    char pin_dir[PATH_MAX];
} ebpf_runtime_status_t;

int ebpf_probe(void);
int ebpf_probe_detailed(ebpf_probe_result_t *res);
int ebpf_status(char *state, size_t size, atp_config_t *cfg);
bool ebpf_is_pure_mode(const atp_config_t *cfg);
int ebpf_get_telemetry(atp_ebpf_telemetry_t *stats);
int ebpf_get_runtime_status(ebpf_runtime_status_t *status);

#endif
