#ifndef CONFIG_H
#define CONFIG_H

#include "atp.h"

typedef struct {
    int has_backup;
    char backup_path[PATH_MAX];
    uint64_t version;
    uint64_t load_time;
} config_snapshot_t;

void config_set_defaults(atp_config_t *cfg);
int config_load(const char *path, atp_config_t *cfg);
int config_save_runtime(const char *path, atp_config_t *cfg);
int config_set_mode(atp_config_t *cfg, const char *mode);
int config_reload(atp_config_t *cfg);
int config_reload_atomic(atp_config_t *cfg);
int config_rollback(atp_config_t *cfg);
const config_snapshot_t* config_get_snapshot(void);

int validate_interface_name(const char *name);
int validate_port(int port);
int validate_mark(int mark);

#endif
