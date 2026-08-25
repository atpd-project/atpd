#include "ebpf.h"
#include "ebpf_common.h"
#include "logger.h"
#include "atp_error.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <sys/resource.h>

static void raise_memlock(void) {
    struct rlimit unlimited = {RLIM_INFINITY, RLIM_INFINITY};
    setrlimit(RLIMIT_MEMLOCK, &unlimited);
}

int ebpf_probe_detailed(ebpf_probe_result_t *res) {
    if (!res) return ATP_ERR_INVAL;

    static ebpf_probe_result_t s_cached_probe;
    static bool s_has_cache = false;
    static time_t s_last_cached = 0;
    time_t now = time(NULL);

    if (s_has_cache && (now - s_last_cached) < 60) {
        memcpy(res, &s_cached_probe, sizeof(*res));
        return ATP_OK;
    }

    memset(res, 0, sizeof(*res));

    struct utsname uts;
    if (uname(&uts) == 0) {
        snprintf(res->kernel_release, sizeof(res->kernel_release), "%s", uts.release);
    } else {
        snprintf(res->kernel_release, sizeof(res->kernel_release), "unknown");
    }

    raise_memlock();

    /* 1. Direct kernel eBPF map types probe via sys_bpf syscall */
    res->has_hash = (ebpf_probe_map_type(BPF_MAP_TYPE_HASH, sizeof(uint32_t), sizeof(uint64_t), 16) == 0);
    res->has_array = (ebpf_probe_map_type(BPF_MAP_TYPE_ARRAY, sizeof(uint32_t), sizeof(uint64_t), 16) == 0);
    res->has_lru_hash = (ebpf_probe_map_type(BPF_MAP_TYPE_LRU_HASH, sizeof(uint32_t), sizeof(uint64_t), 16) == 0);
    res->has_lpm_trie = (ebpf_probe_map_type(BPF_MAP_TYPE_LPM_TRIE, sizeof(struct sb_ebpf_uid_lpm_key), sizeof(uint8_t), 16) == 0);

    /* 2. Direct program types probe */
    res->has_cgroup_sock_addr = (ebpf_probe_prog_type(BPF_PROG_TYPE_CGROUP_SOCK_ADDR) == 0);
    res->has_sched_cls = (ebpf_probe_prog_type(BPF_PROG_TYPE_SCHED_CLS) == 0);

    /* A failed syscall is not evidence of support: it may be EPERM, which
     * must remain visible to callers instead of being reported as ready. */
    res->supported = (res->has_hash && res->has_array &&
                      res->has_cgroup_sock_addr);

    memcpy(&s_cached_probe, res, sizeof(*res));
    s_has_cache = true;
    s_last_cached = now;

    return ATP_OK;
}

int ebpf_probe(void) {
    ebpf_probe_result_t res;
    if (ebpf_probe_detailed(&res) != ATP_OK) {
        return ATP_ERR_EBPF;
    }
    return res.supported ? ATP_OK : ATP_ERR_EBPF;
}

int ebpf_status(char *state, size_t size, atp_config_t *cfg) {
    if (!state || size == 0) {
        return ATP_ERR_INVAL;
    }

    if (!cfg || !cfg->ebpf.enabled) {
        snprintf(state, size, "%s", "disabled");
        return ATP_OK;
    }

    ebpf_probe_result_t res;
    if (ebpf_probe_detailed(&res) == ATP_OK && res.supported) {
        snprintf(state, size, "%s", "ready");
        return ATP_OK;
    }

    snprintf(state, size, "%s", "unsupported");
    return ATP_ERR_EBPF;
}

bool ebpf_is_pure_mode(const atp_config_t *cfg) {
    (void)cfg;
    return true;
}

int ebpf_get_telemetry(atp_ebpf_telemetry_t *stats) {
    if (!stats) return -1;
    memset(stats, 0, sizeof(*stats));

    static atp_ebpf_telemetry_t s_cached_stats;
    static bool s_has_stats_cache = false;
    static time_t s_last_stats_check = 0;
    time_t now = time(NULL);

    if (s_has_stats_cache && (now - s_last_stats_check) < 2) {
        memcpy(stats, &s_cached_stats, sizeof(*stats));
        return 0;
    }

    /* 1. Direct Zero-Context-Switch BPF Map lookup via ebpf_call */
    const char *pinned_maps[] = {
        "/sys/fs/bpf/atp/stats_map",
        "/sys/fs/bpf/atpd_stats",
        "/sys/fs/bpf/sb_ebpf_stats"
    };

    for (size_t i = 0; i < sizeof(pinned_maps)/sizeof(pinned_maps[0]); i++) {
        if (access(pinned_maps[i], R_OK) == 0) {
            union bpf_attr attr;
            memset(&attr, 0, sizeof(attr));
            attr.pathname = (uint64_t)(uintptr_t)pinned_maps[i];

            long map_fd = ebpf_call(BPF_OBJ_GET, &attr);
            if (map_fd >= 0) {
                uint32_t key = 0;
                uint64_t values[4] = {0};
                union bpf_attr lookup_attr;
                memset(&lookup_attr, 0, sizeof(lookup_attr));
                lookup_attr.map_fd = (uint32_t)map_fd;
                lookup_attr.key = (uint64_t)(uintptr_t)&key;
                lookup_attr.value = (uint64_t)(uintptr_t)values;

                if (ebpf_call(BPF_MAP_LOOKUP_ELEM, &lookup_attr) == 0) {
                    stats->total_packets = values[0];
                    stats->total_bytes = values[1];
                    stats->active_conns = values[2];
                    stats->dns_intercepts = values[3];
                    stats->map_direct = true;
                    close((int)map_fd);

                    memcpy(&s_cached_stats, stats, sizeof(*stats));
                    s_has_stats_cache = true;
                    s_last_stats_check = now;
                    return 0;
                }
                close((int)map_fd);
            }
        }
    }

    /* 2. Kernel procfs telemetry fallback */
    int pid = get_pid_by_name("sing-box");
    if (pid > 0) {
        stats->active_conns = (uint64_t)get_process_fd_count(pid);
    }
    stats->map_direct = false;

    memcpy(&s_cached_stats, stats, sizeof(*stats));
    s_has_stats_cache = true;
    s_last_stats_check = now;
    return 0;
}

int ebpf_get_runtime_status(ebpf_runtime_status_t *status) {
    if (!status) return -1;
    memset(status, 0, sizeof(*status));

    static ebpf_runtime_status_t s_cached_runtime;
    static bool s_has_runtime_cache = false;
    static time_t s_last_runtime_check = 0;
    time_t now = time(NULL);

    if (s_has_runtime_cache && (now - s_last_runtime_check) < 2) {
        memcpy(status, &s_cached_runtime, sizeof(*status));
        return 0;
    }

    const char *candidate_dirs[] = {
        "/sys/fs/bpf/sing-box",
        "/sys/fs/bpf/atp",
        "/sys/fs/bpf"
    };

    for (size_t i = 0; i < sizeof(candidate_dirs)/sizeof(candidate_dirs[0]); i++) {
        char ctrl_path[PATH_MAX];
        snprintf(ctrl_path, sizeof(ctrl_path), "%s/control_map", candidate_dirs[i]);

        if (access(ctrl_path, R_OK) == 0) {
            union bpf_attr attr;
            memset(&attr, 0, sizeof(attr));
            attr.pathname = (uint64_t)(uintptr_t)ctrl_path;

            long fd = ebpf_call(BPF_OBJ_GET, &attr);
            if (fd >= 0) {
                uint32_t key = 0;
                struct sb_ebpf_cgroup_control ctrl;
                memset(&ctrl, 0, sizeof(ctrl));

                union bpf_attr lookup_attr;
                memset(&lookup_attr, 0, sizeof(lookup_attr));
                lookup_attr.map_fd = (uint32_t)fd;
                lookup_attr.key = (uint64_t)(uintptr_t)&key;
                lookup_attr.value = (uint64_t)(uintptr_t)&ctrl;

                if (ebpf_call(BPF_MAP_LOOKUP_ELEM, &lookup_attr) == 0) {
                    status->is_active = true;
                    status->flags = ctrl.flags;
                    status->self_tgid = ctrl.self_tgid;
                    status->listener_port = ctrl.listener_port;
                    snprintf(status->pin_dir, sizeof(status->pin_dir), "%s", candidate_dirs[i]);
                }
                close((int)fd);

                char flow_path[PATH_MAX];
                snprintf(flow_path, sizeof(flow_path), "%s/flow_map", candidate_dirs[i]);
                if (access(flow_path, R_OK) == 0) {
                    memset(&attr, 0, sizeof(attr));
                    attr.pathname = (uint64_t)(uintptr_t)flow_path;
                    long flow_fd = ebpf_call(BPF_OBJ_GET, &attr);
                    if (flow_fd >= 0) {
                        struct sb_ebpf_udp_flow_key cur_key = {0}, next_key;
                        uint32_t count = 0;
                        union bpf_attr next_attr;
                        memset(&next_attr, 0, sizeof(next_attr));
                        next_attr.map_fd = (uint32_t)flow_fd;
                        next_attr.key = 0;
                        next_attr.next_key = (uint64_t)(uintptr_t)&next_key;

                        while (ebpf_call(BPF_MAP_GET_NEXT_KEY, &next_attr) == 0) {
                            count++;
                            cur_key = next_key;
                            next_attr.key = (uint64_t)(uintptr_t)&cur_key;
                        }
                        status->active_flows = count;
                        close((int)flow_fd);
                    }
                }

                if (status->is_active) {
                    memcpy(&s_cached_runtime, status, sizeof(*status));
                    s_has_runtime_cache = true;
                    s_last_runtime_check = now;
                    return 0;
                }
            }
        }
    }

    return -1;
}
