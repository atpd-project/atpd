#ifndef ATP_IPSET_H
#define ATP_IPSET_H

#include "atp.h"

typedef struct {
    char name[64];
    int family;
    int hashsize;
    int maxelem;
    char filename[PATH_MAX];
    time_t last_update;
} ipset_info_t;

int ipset_init(atp_config_t *cfg);
int ipset_create(const char *name, int family, int hashsize, int maxelem);
int ipset_destroy(const char *name);
int ipset_flush(const char *name);
int ipset_swap(const char *from, const char *to);
int ipset_exists(const char *name);
int ipset_add_entry(const char *name, const char *entry);
int ipset_del_entry(const char *name, const char *entry);
int ipset_list_entries(const char *name, char *output, size_t size);
int ipset_save(const char *name, const char *filename);
int ipset_restore(const char *filename);
int ipset_restore_file(const char *name, const char *filename);
int ipset_atomic_update(const char *name, const char *filename, int family, int hashsize, int maxelem);

int ipset_add_cidr_list(const char *name, const char *cidr_file);
int ipset_parse_cidr_file(const char *input_path, const char *output_path, int family);
int ipset_get_entry_count(const char *name, int *count);

const char *ipset_family_to_string(int family);
int ipset_string_to_family(const char *str);

#endif