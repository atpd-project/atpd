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

    struct utsname uts;
    if (uname(&uts) == 0) {
        snprintf(res->kernel_release, sizeof(res->kernel_release), "%s", uts.release);
    } else {
        snprintf(res->kernel_release, sizeof(res->kernel_release), "unknown");
    }

    raise_memlock();

    /* Probe kernel eBPF map types without persisting anything */
    res->has_hash = (ebpf_probe_map_type(BPF_MAP_TYPE_HASH, sizeof(uint32_t), sizeof(uint64_t), 16) == 0);
    res->has_array = (ebpf_probe_map_type(BPF_MAP_TYPE_ARRAY, sizeof(uint32_t), sizeof(uint64_t), 16) == 0);
    res->has_lru_hash = (ebpf_probe_map_type(BPF_MAP_TYPE_LRU_HASH, sizeof(uint32_t), sizeof(uint64_t), 16) == 0);
    res->has_lpm_trie = (ebpf_probe_map_type(BPF_MAP_TYPE_LPM_TRIE, sizeof(struct sb_ebpf_uid_lpm_key), sizeof(uint8_t), 16) == 0);

    /* Probe program types */
    res->has_cgroup_sock_addr = (ebpf_probe_prog_type(BPF_PROG_TYPE_CGROUP_SOCK_ADDR) == 0);
    res->has_sched_cls = (ebpf_probe_prog_type(BPF_PROG_TYPE_SCHED_CLS) == 0);

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
    if (!cfg) return false;
    if (cfg->network.proxy_mode == MODE_EBPF) {
        return true;
    }
    if (cfg->network.proxy_mode == MODE_AUTO) {
        if (cfg->ebpf.enabled && cfg->ebpf.ready) {
            return true;
        }
    }
    return false;
}

