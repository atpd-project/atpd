#include "ebpf.h"
#include "ebpf_common.h"
#include "logger.h"
#include "atp_result.h"
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

atp_result_t ebpf_probe_detailed(ebpf_probe_result_t *res) {
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
        snprintf(res->kernel_release, sizeof(res->kernel_release), "%.63s", uts.release);
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

atp_result_t ebpf_probe(void) {
    ebpf_probe_result_t res;
    atp_result_t result = ebpf_probe_detailed(&res);
    if (result != ATP_OK) {
        return result;
    }
    return res.supported ? ATP_OK : ATP_ERR_NOTSUP;
}

atp_result_t ebpf_status(char *state, size_t size) {
    if (!state || size == 0) {
        return ATP_ERR_INVAL;
    }

    ebpf_probe_result_t res;
    if (ebpf_probe_detailed(&res) == ATP_OK && res.supported) {
        snprintf(state, size, "%s", "ready");
        return ATP_OK;
    }

    snprintf(state, size, "%s", "unsupported");
    return ATP_ERR_NOTSUP;
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

    /* sing-box owns its eBPF maps and does not expose a stable pinned-map ABI. */
    int pid = get_pid_by_name("sing-box");
    if (pid > 0) {
        stats->active_conns = (uint64_t)get_process_fd_count(pid);
    }
    memcpy(&s_cached_stats, stats, sizeof(*stats));
    s_has_stats_cache = true;
    s_last_stats_check = now;
    return 0;
}
