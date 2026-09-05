#ifndef CONFIG_H
#define CONFIG_H

#include "atp.h"

void config_set_defaults(atp_config_t *cfg);
atp_result_t config_load(const char *path, atp_config_t *cfg);
atp_result_t config_prepare_reload(const char *source_path, atp_config_t *candidate);

typedef enum {
    CONFIG_RELOAD_CHANGE_NONE = 0,
    CONFIG_RELOAD_CHANGE_HOT = 1u << 0,
    CONFIG_RELOAD_CHANGE_STATIC = 1u << 1,
    CONFIG_RELOAD_CHANGE_REQUIRES_RESTART = 1u << 2
} config_reload_changes_t;

config_reload_changes_t config_classify_reload(const atp_config_t *current,
                                               const atp_config_t *candidate,
                                               char *restart_fields,
                                               size_t restart_fields_size);

/* Parse a config file into a caller-owned value. */
atp_result_t config_load_file(const char *path, atp_config_t *cfg);

atp_result_t validate_interface_name(const char *name);
atp_result_t validate_port(int port);
atp_result_t validate_mark(int mark);

#endif
