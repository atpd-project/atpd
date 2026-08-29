#ifndef CONFIG_H
#define CONFIG_H

#include "atp.h"

void config_set_defaults(atp_config_t *cfg);
atp_result_t config_load(const char *path, atp_config_t *cfg);
atp_result_t config_reload(atp_config_t *cfg);

/* Parse a config file into a caller-owned value. */
atp_result_t config_load_file(const char *path, atp_config_t *cfg);

atp_result_t validate_interface_name(const char *name);
atp_result_t validate_port(int port);
atp_result_t validate_mark(int mark);

#endif
