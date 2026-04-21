#include "ipset.h"
#include "logger.h"
#include "utils.h"
#include "atp.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <arpa/inet.h>
#include <stdio.h>

#define IPSET_CMD "/system/bin/ipset"
#define SAFE_PATH_MAX (PATH_MAX + 256)
#define SAFE_CMD_LEN (MAX_CMD_LEN + 256)

static int exec_ipset(atp_config_t *cfg, const char *cmd, const char *arg) {
    if (cfg && cfg->dry_run) {
        LOG_DEBUG("[DRY_RUN] ipset %s %s", cmd, arg ? arg : "");
        return 0;
    }
    
    char command[SAFE_CMD_LEN];
    if (arg) {
        /* Use precision specifiers to guarantee buffer safety for GCC 15 */
        snprintf(command, sizeof(command), "%s %.511s %.511s 2>/dev/null", IPSET_CMD, cmd, arg);
    } else {
        snprintf(command, sizeof(command), "%s %.511s 2>/dev/null", IPSET_CMD, cmd);
    }
    
    return exec_cmd_simple(command, CMD_TIMEOUT_SEC);
}

int ipset_init(atp_config_t *cfg) {
    char rules_dir[SAFE_PATH_MAX];
    if (snprintf(rules_dir, sizeof(rules_dir), "%s/rules", cfg->data_dir) < (int)sizeof(rules_dir)) {
        mkdir_recursive(rules_dir, 0755);
    }
    
    LOG_DEBUG("IPSet initialized");
    return 0;
}

int ipset_create(const char *name, int family, int hashsize, int maxelem) {
    const char *family_str = (family == 4) ? "inet" : "inet6";
    char cmd[512];
    
    snprintf(cmd, sizeof(cmd), "create %.63s hash:net family %s hashsize %d maxelem %d",
             name, family_str, hashsize, maxelem);
    
    return exec_ipset(NULL, cmd, NULL);
}

int ipset_destroy(const char *name) {
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "destroy %.63s", name);
    return exec_ipset(NULL, cmd, NULL);
}

int ipset_flush(const char *name) {
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "flush %.63s", name);
    return exec_ipset(NULL, cmd, NULL);
}

int ipset_swap(const char *from, const char *to) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "swap %.63s %.63s", from, to);
    return exec_ipset(NULL, cmd, NULL);
}

int ipset_exists(const char *name) {
    char cmd[256];
    char output[256];
    
    snprintf(cmd, sizeof(cmd), "list %.63s 2>/dev/null | head -1", name);
    
    if (exec_cmd(cmd, output, sizeof(output), 5) == 0 && output[0] != '\0') {
        return 1;
    }
    return 0;
}

int ipset_add_entry(const char *name, const char *entry) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "add %.63s %.255s -exist", name, entry);
    return exec_ipset(NULL, cmd, NULL);
}

int ipset_del_entry(const char *name, const char *entry) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "del %.63s %.255s", name, entry);
    return exec_ipset(NULL, cmd, NULL);
}

int ipset_list_entries(const char *name, char *output, size_t size) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), 
             "list %.63s 2>/dev/null | grep -v '^Name:\\|^Type:\\|^Revision:\\|^Header:\\|^Size in memory:\\|^References:\\|^Number of entries:' | grep -v '^$'", 
             name);
    
    return exec_cmd(cmd, output, size, 10);
}

int ipset_save(const char *name, const char *filename) {
    char cmd[SAFE_CMD_LEN];
    snprintf(cmd, sizeof(cmd), "save %.63s > %.1023s 2>/dev/null", name, filename);
    return exec_cmd_simple(cmd, 10);
}

int ipset_restore(const char *filename) {
    char cmd[SAFE_CMD_LEN];
    snprintf(cmd, sizeof(cmd), "restore < %.1023s 2>/dev/null", filename);
    return exec_cmd_simple(cmd, 30);
}

int ipset_restore_file(const char *name, const char *filename) {
    char cmd[SAFE_CMD_LEN];
    snprintf(cmd, sizeof(cmd), 
             "awk '!/^[[:space:]]*#/ && NF > 0 {printf \"add %.63s %%s\\n\", $0}' %.1023s | ipset restore -exist 2>/dev/null",
             name, filename);
    return exec_cmd_simple(cmd, 30);
}

int ipset_atomic_update(const char *name, const char *filename, int family, int hashsize, int maxelem) {
    char temp_name[128];
    snprintf(temp_name, sizeof(temp_name), "%.63s_temp", name);
    
    if (ipset_create(temp_name, family, hashsize, maxelem) != 0) {
        LOG_ERROR("Failed to create temporary ipset: %s", temp_name);
        return -1;
    }
    
    if (ipset_restore_file(temp_name, filename) != 0) {
        LOG_ERROR("Failed to restore entries to temporary ipset");
        ipset_destroy(temp_name);
        return -1;
    }
    
    if (ipset_exists(name)) {
        if (ipset_swap(temp_name, name) != 0) {
            LOG_ERROR("Failed to swap ipsets");
            ipset_destroy(temp_name);
            return -1;
        }
        LOG_INFO("IPSet %s swapped atomically", name);
    } else {
        /* If base set doesn't exist, we use rename/swap logic consistently */
        if (ipset_swap(temp_name, name) != 0) {
            LOG_ERROR("Failed to rename temporary ipset");
            ipset_destroy(temp_name);
            return -1;
        }
        LOG_INFO("IPSet %s created", name);
    }
    
    ipset_destroy(temp_name);
    return 0;
}

int ipset_add_cidr_list(const char *name, const char *cidr_file) {
    char cmd[SAFE_CMD_LEN];
    snprintf(cmd, sizeof(cmd), 
             "awk '!/^[[:space:]]*#/ && NF > 0 {printf \"add %.63s %%s\\n\", $0}' %.1023s | ipset restore -exist 2>/dev/null",
             name, cidr_file);
    return exec_cmd_simple(cmd, 30);
}

int ipset_parse_cidr_file(const char *input_path, const char *output_path, int family) {
    FILE *fin = fopen(input_path, "r");
    if (!fin) return -1;
    
    FILE *fout = fopen(output_path, "w");
    if (!fout) {
        fclose(fin);
        return -1;
    }
    
    char line[256];
    struct in_addr ipv4;
    struct in6_addr ipv6;
    
    while (fgets(line, sizeof(line), fin)) {
        trim(line);
        if (line[0] == '#' || line[0] == '\0') continue;
        
        char ip[128];
        int prefix = 0;
        
        if (sscanf(line, "%127[^/]/%d", ip, &prefix) == 2) {
            int valid = 0;
            if (family == 4) {
                if (inet_pton(AF_INET, ip, &ipv4) == 1 && prefix >= 0 && prefix <= 32) {
                    valid = 1;
                }
            } else {
                if (inet_pton(AF_INET6, ip, &ipv6) == 1 && prefix >= 0 && prefix <= 128) {
                    valid = 1;
                }
            }
            
            if (valid) {
                fprintf(fout, "%s\n", line);
            }
        }
    }
    
    fclose(fin);
    fclose(fout);
    return 0;
}

int ipset_get_entry_count(const char *name, int *count) {
    char cmd[256];
    char output[256];
    
    snprintf(cmd, sizeof(cmd), "list %.63s 2>/dev/null | grep -c '^add'", name);
    
    if (exec_cmd(cmd, output, sizeof(output), 5) == 0) {
        *count = atoi(output);
        return 0;
    }
    
    return -1;
}

const char *ipset_family_to_string(int family) {
    return (family == 4) ? "inet" : "inet6";
}

int ipset_string_to_family(const char *str) {
    if (str && strcmp(str, "inet6") == 0) return 6;
    return 4;
}
