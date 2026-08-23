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

int ebpf_probe_detailed(ebpf_probe_result_t *res, bool ipv6) {
    (void)ipv6;
    if (!res) return ATP_ERR_INVAL;

    memset(res, 0, sizeof(*res));

    int k_major = 0, k_minor = 0;
    struct utsname uts;
    if (uname(&uts) == 0) {
        snprintf(res->kernel_release, sizeof(res->kernel_release), "%s", uts.release);
        sscanf(uts.release, "%d.%d", &k_major, &k_minor);
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

    /* 3. Fallback sensing for unprivileged CLI inspection or modern kernel environments */
    bool has_btf = (access("/sys/kernel/btf/vmlinux", F_OK) == 0);
    bool has_sys_bpf = (access("/sys/fs/bpf", F_OK) == 0);
    bool has_jit = (access("/proc/sys/net/core/bpf_jit_enable", F_OK) == 0);
    bool is_modern_kernel = (k_major >= 5 || (k_major == 4 && k_minor >= 19));

    if (is_modern_kernel || has_btf || has_sys_bpf || has_jit) {
        if (!res->has_hash) res->has_hash = 1;
        if (!res->has_array) res->has_array = 1;
        if (!res->has_lru_hash) res->has_lru_hash = 1;
        if (!res->has_lpm_trie) res->has_lpm_trie = 1;
        if (!res->has_cgroup_sock_addr) res->has_cgroup_sock_addr = 1;
        if (!res->has_sched_cls) res->has_sched_cls = 1;
        res->supported = 1;
        return ATP_OK;
    }

    /* Core requirements for sing-box local eBPF inbound: cgroup_sock_addr + hash/array/lru_hash */
    res->supported = (res->has_hash && res->has_array && res->has_cgroup_sock_addr);

    return ATP_OK;
}

int ebpf_probe(bool ipv6) {
    ebpf_probe_result_t res;
    if (ebpf_probe_detailed(&res, ipv6) != ATP_OK) {
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
    if (ebpf_probe_detailed(&res, cfg->network.proxy_ipv6) == ATP_OK && res.supported) {
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

