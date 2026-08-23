#include "ebpf_common.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

long ebpf_call(enum bpf_cmd cmd, union bpf_attr *attr) {
    return syscall(__NR_bpf, cmd, attr, sizeof(*attr));
}

int ebpf_probe_map_type(enum bpf_map_type type, size_t key_size, size_t value_size, uint32_t max_entries) {
    union bpf_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.map_type = type;
    attr.key_size = (uint32_t)key_size;
    attr.value_size = (uint32_t)value_size;
    attr.max_entries = max_entries;
    if (type == BPF_MAP_TYPE_LPM_TRIE) {
        attr.map_flags = BPF_F_NO_PREALLOC;
    }

    long ret = ebpf_call(BPF_MAP_CREATE, &attr);
    if (ret >= 0) {
        close((int)ret);
        return 0;
    }
    return -1;
}

int ebpf_probe_prog_type(enum bpf_prog_type type) {
    /* Minimal valid BPF program */
    int ret_val = (type == BPF_PROG_TYPE_CGROUP_SOCK_ADDR) ? 1 : 0;
    struct bpf_insn insns[] = {
        { .code = 0xb7, .dst_reg = 0, .src_reg = 0, .off = 0, .imm = ret_val }, /* BPF_MOV64_IMM(BPF_REG_0, ret_val) */
        { .code = 0x95, .dst_reg = 0, .src_reg = 0, .off = 0, .imm = 0 }        /* BPF_EXIT_INSN() */
    };
    char log_buf[512] = {0};

    union bpf_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.prog_type = type;
    if (type == BPF_PROG_TYPE_CGROUP_SOCK_ADDR) {
        attr.expected_attach_type = BPF_CGROUP_INET4_CONNECT;
    }
    attr.insns = (uint64_t)(uintptr_t)insns;
    attr.insn_cnt = sizeof(insns) / sizeof(insns[0]);
    attr.license = (uint64_t)(uintptr_t)"GPL";
    attr.log_buf = (uint64_t)(uintptr_t)log_buf;
    attr.log_size = sizeof(log_buf);
    attr.log_level = 0;

    long ret = ebpf_call(BPF_PROG_LOAD, &attr);
    if (ret >= 0) {
        close((int)ret);
        return 0;
    }
    return -1;
}
