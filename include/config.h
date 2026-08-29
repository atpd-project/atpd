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
atp_result_t config_load(const char *path, atp_config_t *cfg);
atp_result_t config_save_runtime(const char *path, atp_config_t *cfg);
atp_result_t config_set_mode(atp_config_t *cfg, const char *mode);
atp_result_t config_reload(atp_config_t *cfg);
atp_result_t config_reload_atomic(atp_config_t *cfg);
atp_result_t config_rollback(atp_config_t *cfg);
atp_result_t config_get_snapshot(config_snapshot_t *out);
 
/* Load config file without mutex */
atp_result_t config_load_file(const char *path, atp_config_t *cfg);

atp_result_t validate_interface_name(const char *name);
atp_result_t validate_port(int port);
atp_result_t validate_mark(int mark);

#endif
