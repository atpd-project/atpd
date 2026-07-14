#ifndef ATP_BPF_COMMON_H
#define ATP_BPF_COMMON_H

#include "compat.h"
#include <linux/bpf.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define PIN_DIR "/sys/fs/bpf/box"

#define PIN_CIDR_OUT4 PIN_DIR "/box_cidr_out4"
#define PIN_CIDR_OUT6 PIN_DIR "/box_cidr_out6"
#define PIN_CIDR_PRE4 PIN_DIR "/box_cidr_pre4"
#define PIN_CIDR_PRE6 PIN_DIR "/box_cidr_pre6"
#define PIN_FORCE_OUT4 PIN_DIR "/box_force_out4"
#define PIN_FORCE_OUT6 PIN_DIR "/box_force_out6"
#define PIN_APP_OUT4 PIN_DIR "/box_uid_out4"
#define PIN_APP_OUT6 PIN_DIR "/box_uid_out6"

#define MAP_CIDR4 PIN_DIR "/box_cidr4_lpm"
#define MAP_CIDR6 PIN_DIR "/box_cidr6_lpm"
#define MAP_FORCE_UID PIN_DIR "/box_force_uid_set"
#define MAP_APP_UID PIN_DIR "/box_app_uid_set"

#define PROBE_PIN4 PIN_DIR "/box_probe4"
#define PROBE_PIN6 PIN_DIR "/box_probe6"
#define PROBE_MAP_CIDR4 PIN_DIR "/box_probe_cidr4"
#define PROBE_MAP_CIDR6 PIN_DIR "/box_probe_cidr6"
#define PROBE_MAP_FORCE_UID PIN_DIR "/box_probe_force_uid"
#define PROBE_MAP_APP_UID PIN_DIR "/box_probe_app_uid"

#define MAX_CIDRS 65536U
#define MAX_UIDS 8192U
#define MAX_PATH_LEN 512
#define VERIFY_LOG_SIZE 65536

struct lpm4_key {
    uint32_t prefixlen;
    uint8_t addr[4];
};

struct lpm6_key {
    uint32_t prefixlen;
    uint8_t addr[16];
};

long bpf_call(enum bpf_cmd cmd, union bpf_attr *attr);
int create_map(enum bpf_map_type type, size_t key_size, size_t value_size,
               uint32_t max_entries, uint32_t flags);
int update_elem(int fd, const void *key, const void *value);
int clear_map(int fd, size_t key_size);
int get_pinned(const char *path);
int pin_replace(int fd, const char *path);
void close_fd(int fd);
void remove_known_pins(void);

int load_program(const char *section_name, const char *program_name,
                 int cidr4_fd, int cidr6_fd, int force_uid_fd, int app_uid_fd);

int load_cidr_file(int map_fd, const char *path, bool ipv6);
int load_uid_file(int map_fd, const char *path);
bool uid_file_has_entries(const char *path);

#endif
