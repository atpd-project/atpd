#include "utils.h"
#include "bpf_common.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

long bpf_call(enum bpf_cmd cmd, union bpf_attr *attr) {
    return syscall(__NR_bpf, cmd, attr, sizeof(*attr));
}

int create_map(enum bpf_map_type type, size_t key_size, size_t value_size,
               uint32_t max_entries, uint32_t flags) {
    union bpf_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.map_type = type;
    attr.key_size = (uint32_t)key_size;
    attr.value_size = (uint32_t)value_size;
    attr.max_entries = max_entries;
    attr.map_flags = flags;
    return (int)bpf_call(BPF_MAP_CREATE, &attr);
}

int update_elem(int fd, const void *key, const void *value) {
    union bpf_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.map_fd = (uint32_t)fd;
    attr.key = (uint64_t)(uintptr_t)key;
    attr.value = (uint64_t)(uintptr_t)value;
    attr.flags = BPF_ANY;
    return (int)bpf_call(BPF_MAP_UPDATE_ELEM, &attr);
}

static int delete_elem(int fd, const void *key) {
    union bpf_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.map_fd = (uint32_t)fd;
    attr.key = (uint64_t)(uintptr_t)key;
    return (int)bpf_call(BPF_MAP_DELETE_ELEM, &attr);
}

static int next_key(int fd, const void *key, void *next) {
    union bpf_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.map_fd = (uint32_t)fd;
    attr.key = (uint64_t)(uintptr_t)key;
    attr.next_key = (uint64_t)(uintptr_t)next;
    return (int)bpf_call(BPF_MAP_GET_NEXT_KEY, &attr);
}

int clear_map(int fd, size_t key_size) {
    uint8_t *key = calloc(1, key_size);
    uint8_t *next = calloc(1, key_size);
    if (!key || !next) {
        free(key);
        free(next);
        return -1;
    }

    int count = 0;
    if (next_key(fd, NULL, next) == 0) {
        do {
            memcpy(key, next, key_size);
            if (delete_elem(fd, key) != 0) {
                free(key);
                free(next);
                return -1;
            }
            ++count;
        } while (next_key(fd, key, next) == 0);
    }

    free(key);
    free(next);
    return count;
}

int get_pinned(const char *path) {
    union bpf_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.pathname = (uint64_t)(uintptr_t)path;
    return (int)bpf_call(BPF_OBJ_GET, &attr);
}

static int ensure_parent_dir(const char *path) {
    char dir[MAX_PATH_LEN];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash) return 0;
    *slash = '\0';
    return mkdir_recursive(dir, 0700);
}

int pin_replace(int fd, const char *path) {
    char tmp[MAX_PATH_LEN];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    if (ensure_parent_dir(path) != 0) {
        fprintf(stderr, "pin_replace: failed to create parent dir for %s\n", path);
        return -1;
    }

    unlink(tmp);

    union bpf_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.pathname = (uint64_t)(uintptr_t)tmp;
    attr.bpf_fd = (uint32_t)fd;
    int rc = (int)bpf_call(BPF_OBJ_PIN, &attr);
    if (rc != 0) {
        fprintf(stderr, "pin BPF object failed: %s errno=%d (%s)\n",
                tmp, errno, strerror(errno));
        return rc;
    }

    if (rename(tmp, path) != 0) {
        fprintf(stderr, "rename %s -> %s failed: %d (%s)\n",
                tmp, path, errno, strerror(errno));
        unlink(tmp);
        return -1;
    }

    return 0;
}

void close_fd(int fd) {
    if (fd >= 0) close(fd);
}

void remove_known_pins(void) {
    const char *pins[] = {
        PIN_CIDR_OUT4, PIN_CIDR_OUT6,
        PIN_CIDR_PRE4, PIN_CIDR_PRE6,
        PIN_FORCE_OUT4, PIN_FORCE_OUT6,
        PIN_APP_OUT4, PIN_APP_OUT6,
        MAP_CIDR4, MAP_CIDR6,
        MAP_FORCE_UID, MAP_APP_UID,
        PROBE_PIN4, PROBE_PIN6,
        PROBE_MAP_CIDR4, PROBE_MAP_CIDR6,
        PROBE_MAP_FORCE_UID, PROBE_MAP_APP_UID,
    };

    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        unlink(pins[i]);
    }
    rmdir(PIN_DIR);
}

#define INSN_MOV64_REG(D, S) \
    ((struct bpf_insn){.code = BPF_ALU64 | BPF_MOV | BPF_X, .dst_reg = (D), .src_reg = (S)})
#define INSN_MOV64_IMM(D, V) \
    ((struct bpf_insn){.code = BPF_ALU64 | BPF_MOV | BPF_K, .dst_reg = (D), .imm = (V)})
#define INSN_ALU64_IMM(OP, D, V) \
    ((struct bpf_insn){.code = BPF_ALU64 | (OP) | BPF_K, .dst_reg = (D), .imm = (V)})
#define INSN_ST_MEM(SZ, D, O, V) \
    ((struct bpf_insn){.code = BPF_ST | (SZ) | BPF_MEM, .dst_reg = (D), .off = (O), .imm = (V)})
#define INSN_STX_MEM(SZ, D, S, O) \
    ((struct bpf_insn){.code = BPF_STX | (SZ) | BPF_MEM, .dst_reg = (D), .src_reg = (S), .off = (O)})
#define INSN_LDX_MEM(SZ, D, S, O) \
    ((struct bpf_insn){.code = BPF_LDX | (SZ) | BPF_MEM, .dst_reg = (D), .src_reg = (S), .off = (O)})
#define INSN_LD_ABS(SZ, O) \
    ((struct bpf_insn){.code = BPF_LD | (SZ) | BPF_ABS, .imm = (O)})
#define INSN_JMP_IMM(OP, D, V, O) \
    ((struct bpf_insn){.code = BPF_JMP | (OP) | BPF_K, .dst_reg = (D), .off = (O), .imm = (V)})
#define INSN_JMP_REG(OP, D, S, O) \
    ((struct bpf_insn){.code = BPF_JMP | (OP) | BPF_X, .dst_reg = (D), .src_reg = (S), .off = (O)})
#define INSN_CALL(F) \
    ((struct bpf_insn){.code = BPF_JMP | BPF_CALL, .imm = (F)})
#define INSN_EXIT() \
    ((struct bpf_insn){.code = BPF_JMP | BPF_EXIT})

#define XT_BPF_MISS 0U
#define XT_BPF_MATCH 0xffffU
#define MAX_PROGRAM_INSNS 160
#define NO_JUMP ((size_t)-1)

enum read_mode {
    READ_DIRECT,
    READ_HELPER,
    READ_LDABS,
};

struct packet_layout {
    int key_stack_off;
    int addr_stack_off;
    int scratch_stack_off;
    int packet_addr_off;
    int addr_len;
    int full_prefix;
};

static const struct packet_layout LAYOUT_V4 = {-8, -4, -12, 16, 4, 32};
static const struct packet_layout LAYOUT_V6 = {-24, -20, -28, 24, 16, 128};

struct prog_builder {
    struct bpf_insn insns[MAX_PROGRAM_INSNS];
    size_t count;
    bool overflow;
};

static void emit(struct prog_builder *b, struct bpf_insn insn) {
    if (b->count >= MAX_PROGRAM_INSNS) {
        b->overflow = true;
        return;
    }
    b->insns[b->count++] = insn;
}

static size_t emit_jump(struct prog_builder *b, struct bpf_insn insn) {
    size_t index = b->count;
    emit(b, insn);
    return index;
}

static void patch_jump_here(struct prog_builder *b, size_t jump_index) {
    if (jump_index < b->count) {
        b->insns[jump_index].off = (int16_t)(b->count - jump_index - 1U);
    }
}

static void emit_map_fd(struct prog_builder *b, int reg, int map_fd) {
    emit(b, (struct bpf_insn){
        .code = BPF_LD | BPF_DW | BPF_IMM,
        .dst_reg = (uint8_t)reg,
        .src_reg = COMPAT_PSEUDO_MAP_FD,
        .imm = map_fd,
    });
    emit(b, (struct bpf_insn){0});
}

static void emit_result(struct prog_builder *b, uint32_t value) {
    emit(b, INSN_MOV64_IMM(BPF_REG_0, (int)value));
    emit(b, INSN_EXIT());
}

static size_t emit_load_destination(struct prog_builder *b,
                                    const struct packet_layout *l,
                                    enum read_mode mode) {
    emit(b, INSN_ST_MEM(BPF_W, BPF_REG_10, l->key_stack_off, l->full_prefix));

    if (mode == READ_LDABS) {
        for (int i = 0; i < l->addr_len; ++i) {
            emit(b, INSN_LD_ABS(BPF_B, l->packet_addr_off + i));
            emit(b, INSN_STX_MEM(BPF_B, BPF_REG_10, BPF_REG_0, l->addr_stack_off + i));
        }
        return NO_JUMP;
    }

    if (mode == READ_HELPER) {
        emit(b, INSN_MOV64_REG(BPF_REG_1, BPF_REG_6));
        emit(b, INSN_MOV64_IMM(BPF_REG_2, l->packet_addr_off));
        emit(b, INSN_MOV64_REG(BPF_REG_3, BPF_REG_10));
        emit(b, INSN_ALU64_IMM(BPF_ADD, BPF_REG_3, l->addr_stack_off));
        emit(b, INSN_MOV64_IMM(BPF_REG_4, l->addr_len));
        emit(b, INSN_CALL(COMPAT_SKB_LOAD_BYTES));
        return emit_jump(b, INSN_JMP_IMM(BPF_JNE, BPF_REG_0, 0, 0));
    }

    emit(b, INSN_LDX_MEM(BPF_W, BPF_REG_1, BPF_REG_6,
                         offsetof(struct __sk_buff, data)));
    emit(b, INSN_LDX_MEM(BPF_W, BPF_REG_2, BPF_REG_6,
                         offsetof(struct __sk_buff, data_end)));
    emit(b, INSN_MOV64_REG(BPF_REG_3, BPF_REG_1));
    emit(b, INSN_ALU64_IMM(BPF_ADD, BPF_REG_3, l->packet_addr_off + l->addr_len));
    size_t out_of_bounds = emit_jump(b, INSN_JMP_REG(BPF_JGT, BPF_REG_3, BPF_REG_2, 0));
    emit(b, INSN_ALU64_IMM(BPF_ADD, BPF_REG_1, l->packet_addr_off));
    for (int i = 0; i < l->addr_len; ++i) {
        emit(b, INSN_LDX_MEM(BPF_B, BPF_REG_3, BPF_REG_1, i));
        emit(b, INSN_STX_MEM(BPF_B, BPF_REG_10, BPF_REG_3, l->addr_stack_off + i));
    }
    return out_of_bounds;
}

static void emit_cidr_lookup(struct prog_builder *b, int cidr_map_fd,
                             const struct packet_layout *l) {
    emit_map_fd(b, BPF_REG_1, cidr_map_fd);
    emit(b, INSN_MOV64_REG(BPF_REG_2, BPF_REG_10));
    emit(b, INSN_ALU64_IMM(BPF_ADD, BPF_REG_2, l->key_stack_off));
    emit(b, INSN_CALL(BPF_FUNC_map_lookup_elem));
}

static int load_socket_filter(const struct prog_builder *b, const char *name,
                              bool log_error) {
    static char verifier_log[VERIFY_LOG_SIZE];
    if (b->overflow) {
        if (log_error) {
            fprintf(stderr, "BPF program %s is too large\n", name);
        }
        return -1;
    }

    union bpf_attr attr;
    memset(&attr, 0, sizeof(attr));
    memset(verifier_log, 0, sizeof(verifier_log));
    attr.prog_type = BPF_PROG_TYPE_SOCKET_FILTER;
    attr.insns = (uint64_t)(uintptr_t)b->insns;
    attr.insn_cnt = (uint32_t)b->count;
    attr.license = (uint64_t)(uintptr_t)"GPL";
    attr.log_buf = (uint64_t)(uintptr_t)verifier_log;
    attr.log_size = sizeof(verifier_log);
    attr.log_level = 1;
    snprintf(attr.prog_name, COMPAT_OBJECT_NAME_LEN, "%s", name);

    int fd = (int)bpf_call(BPF_PROG_LOAD, &attr);
    if (fd < 0 && log_error) {
        fprintf(stderr, "eBPF program load rejected: program=%s errno=%d (%s)\n",
                name, errno, strerror(errno));
        if (verifier_log[0] != '\0') {
            fprintf(stderr, "verifier log:\n%s\n", verifier_log);
        }
    }
    return fd;
}

typedef int (*matcher_builder)(int, int, const struct packet_layout *,
                               const char *, enum read_mode, bool);

static int load_with_read_fallback(matcher_builder build, int map_a_fd,
                                   int map_b_fd, const struct packet_layout *l,
                                   const char *name) {
    static const enum read_mode order[] = {READ_DIRECT, READ_HELPER, READ_LDABS};
    const size_t count = sizeof(order) / sizeof(order[0]);
    for (size_t i = 0; i < count; ++i) {
        int fd = build(map_a_fd, map_b_fd, l, name, order[i], i + 1 == count);
        if (fd >= 0) {
            return fd;
        }
    }
    return -1;
}

static int build_cidr_matcher(int cidr_map_fd, int unused_fd,
                              const struct packet_layout *l, const char *name,
                              enum read_mode mode, bool log_error) {
    (void)unused_fd;
    struct prog_builder b = {0};
    emit(&b, INSN_MOV64_REG(BPF_REG_6, BPF_REG_1));

    size_t load_fail = emit_load_destination(&b, l, mode);
    emit_cidr_lookup(&b, cidr_map_fd, l);
    size_t cidr_miss = emit_jump(&b, INSN_JMP_IMM(BPF_JEQ, BPF_REG_0, 0, 0));

    emit_result(&b, XT_BPF_MATCH);
    patch_jump_here(&b, cidr_miss);
    if (load_fail != NO_JUMP) {
        patch_jump_here(&b, load_fail);
    }
    emit_result(&b, XT_BPF_MISS);

    return load_socket_filter(&b, name, log_error);
}

static int load_cidr_matcher(int cidr_map_fd, const struct packet_layout *l,
                             const char *name) {
    return load_with_read_fallback(build_cidr_matcher, cidr_map_fd, -1, l, name);
}

static int build_force_matcher(int cidr_map_fd, int uid_map_fd,
                               const struct packet_layout *l, const char *name,
                               enum read_mode mode, bool log_error) {
    struct prog_builder b = {0};
    emit(&b, INSN_MOV64_REG(BPF_REG_6, BPF_REG_1));

    emit(&b, INSN_MOV64_REG(BPF_REG_1, BPF_REG_6));
    emit(&b, INSN_CALL(COMPAT_GET_SOCKET_UID));
    emit(&b, INSN_STX_MEM(BPF_W, BPF_REG_10, BPF_REG_0, l->scratch_stack_off));
    emit_map_fd(&b, BPF_REG_1, uid_map_fd);
    emit(&b, INSN_MOV64_REG(BPF_REG_2, BPF_REG_10));
    emit(&b, INSN_ALU64_IMM(BPF_ADD, BPF_REG_2, l->scratch_stack_off));
    emit(&b, INSN_CALL(BPF_FUNC_map_lookup_elem));
    size_t uid_miss = emit_jump(&b, INSN_JMP_IMM(BPF_JEQ, BPF_REG_0, 0, 0));

    size_t load_fail = emit_load_destination(&b, l, mode);
    emit_cidr_lookup(&b, cidr_map_fd, l);
    size_t cidr_miss = emit_jump(&b, INSN_JMP_IMM(BPF_JEQ, BPF_REG_0, 0, 0));

    emit_result(&b, XT_BPF_MATCH);
    patch_jump_here(&b, uid_miss);
    patch_jump_here(&b, cidr_miss);
    if (load_fail != NO_JUMP) {
        patch_jump_here(&b, load_fail);
    }
    emit_result(&b, XT_BPF_MISS);

    return load_socket_filter(&b, name, log_error);
}

static int load_force_matcher(int cidr_map_fd, int uid_map_fd,
                              const struct packet_layout *l, const char *name) {
    return load_with_read_fallback(build_force_matcher, cidr_map_fd, uid_map_fd, l, name);
}

static int load_uid_matcher(int uid_map_fd, const char *name) {
    struct prog_builder b = {0};

    emit(&b, INSN_CALL(COMPAT_GET_SOCKET_UID));
    emit(&b, INSN_STX_MEM(BPF_W, BPF_REG_10, BPF_REG_0, -4));
    emit_map_fd(&b, BPF_REG_1, uid_map_fd);
    emit(&b, INSN_MOV64_REG(BPF_REG_2, BPF_REG_10));
    emit(&b, INSN_ALU64_IMM(BPF_ADD, BPF_REG_2, -4));
    emit(&b, INSN_CALL(BPF_FUNC_map_lookup_elem));
    size_t uid_miss = emit_jump(&b, INSN_JMP_IMM(BPF_JEQ, BPF_REG_0, 0, 0));

    emit_result(&b, XT_BPF_MATCH);
    patch_jump_here(&b, uid_miss);
    emit_result(&b, XT_BPF_MISS);

    return load_socket_filter(&b, name, true);
}

int load_program(const char *section_name, const char *program_name,
                 int cidr4_fd, int cidr6_fd, int force_uid_fd, int app_uid_fd) {
    if (strcmp(section_name, "socket/cidr4") == 0) {
        return load_cidr_matcher(cidr4_fd, &LAYOUT_V4, program_name);
    }
    if (strcmp(section_name, "socket/cidr6") == 0) {
        return load_cidr_matcher(cidr6_fd, &LAYOUT_V6, program_name);
    }
    if (strcmp(section_name, "socket/force4") == 0) {
        return load_force_matcher(cidr4_fd, force_uid_fd, &LAYOUT_V4, program_name);
    }
    if (strcmp(section_name, "socket/force6") == 0) {
        return load_force_matcher(cidr6_fd, force_uid_fd, &LAYOUT_V6, program_name);
    }
    if (strcmp(section_name, "socket/appuid") == 0) {
        return load_uid_matcher(app_uid_fd, program_name);
    }

    fprintf(stderr, "unknown BPF program section: %s\n", section_name);
    return -1;
}

static int parse_cidr_line(char *line, uint8_t *address, uint32_t *prefix_len,
                           int *family, bool ipv6) {
    char *trimmed = line;
    while (*trimmed == ' ' || *trimmed == '\t') ++trimmed;
    if (*trimmed == '#' || *trimmed == '\0') return 0;

    size_t len = strlen(trimmed);
    while (len > 0 && (trimmed[len-1] == '\n' || trimmed[len-1] == '\r' ||
                       trimmed[len-1] == ' ' || trimmed[len-1] == '\t')) {
        trimmed[--len] = '\0';
    }
    if (len == 0) return 0;

    char *slash = strchr(trimmed, '/');
    if (!slash) return 0;
    *slash = '\0';

    char *end = NULL;
    unsigned long prefix = strtoul(slash + 1, &end, 10);
    if (end == slash + 1 || *end != '\0') return 0;

    if (strchr(trimmed, ':')) {
        if (prefix > 128UL || inet_pton(AF_INET6, trimmed, address) != 1) return 0;
        *family = AF_INET6;
        *prefix_len = (uint32_t)prefix;
        return ipv6 ? 1 : 0;
    } else {
        if (prefix > 32UL || inet_pton(AF_INET, trimmed, address) != 1) return 0;
        *family = AF_INET;
        *prefix_len = (uint32_t)prefix;
        return !ipv6 ? 1 : 0;
    }
}

int load_cidr_file(int map_fd, const char *path, bool ipv6) {
    FILE *file = fopen(path, "r");
    if (!file) {
        fprintf(stderr, "CIDR file unavailable, using empty map: %s\n", path);
        return 0;
    }

    int loaded = 0;
    int failed = 0;
    char line[256];
    while (fgets(line, sizeof(line), file)) {
        uint8_t address[16] = {0};
        uint32_t prefix_len = 0;
        int family = 0;

        if (parse_cidr_line(line, address, &prefix_len, &family, ipv6) != 1) {
            continue;
        }

        uint8_t value = 1;
        int rc;
        if (ipv6) {
            struct lpm6_key key;
            memset(&key, 0, sizeof(key));
            key.prefixlen = prefix_len;
            memcpy(key.addr, address, sizeof(key.addr));
            rc = update_elem(map_fd, &key, &value);
        } else {
            struct lpm4_key key;
            memset(&key, 0, sizeof(key));
            key.prefixlen = prefix_len;
            memcpy(key.addr, address, sizeof(key.addr));
            rc = update_elem(map_fd, &key, &value);
        }
        if (rc == 0) ++loaded; else ++failed;
    }

    fclose(file);
    if (failed > 0) {
        fprintf(stderr, "CIDR map dropped %d entries (map full or kernel error): %s\n", failed, path);
    }
    return loaded;
}

static int parse_uid_line(char *line, uint32_t *uid_out) {
    char *trimmed = line;
    while (*trimmed == ' ' || *trimmed == '\t') ++trimmed;
    if (*trimmed == '#' || *trimmed == '\0') return 0;

    size_t len = strlen(trimmed);
    while (len > 0 && (trimmed[len-1] == '\n' || trimmed[len-1] == '\r' ||
                       trimmed[len-1] == ' ' || trimmed[len-1] == '\t')) {
        trimmed[--len] = '\0';
    }
    if (len == 0) return 0;

    char *end = NULL;
    unsigned long uid = strtoul(trimmed, &end, 10);
    if (end == trimmed || *end != '\0' || uid > UINT32_MAX) return 0;

    *uid_out = (uint32_t)uid;
    return 1;
}

int load_uid_file(int map_fd, const char *path) {
    if (!path || path[0] == '\0') return 0;

    FILE *file = fopen(path, "r");
    if (!file) {
        fprintf(stderr, "UID file unavailable, using empty map: %s\n", path);
        return 0;
    }

    uint8_t value = 1;
    int loaded = 0;
    int failed = 0;
    char line[128];
    while (fgets(line, sizeof(line), file)) {
        uint32_t uid32;
        if (parse_uid_line(line, &uid32) != 1) {
            continue;
        }

        if (update_elem(map_fd, &uid32, &value) == 0) {
            ++loaded;
        } else {
            ++failed;
        }
    }

    fclose(file);
    if (failed > 0) {
        fprintf(stderr, "UID map dropped %d entries (map full or kernel error): %s\n", failed, path);
    }
    return loaded;
}

bool uid_file_has_entries(const char *path) {
    if (!path || path[0] == '\0') return false;

    FILE *file = fopen(path, "r");
    if (!file) return false;

    char line[128];
    while (fgets(line, sizeof(line), file)) {
        uint32_t uid32;
        if (parse_uid_line(line, &uid32) == 1) {
            fclose(file);
            return true;
        }
    }

    fclose(file);
    return false;
}
