#ifndef ATP_IPV6_MANAGER_H
#define ATP_IPV6_MANAGER_H

#include "atp.h"

typedef enum {
    IPV6_MODE_DEFAULT = 0,
    IPV6_MODE_PROXY = 1,
    IPV6_MODE_DISABLED = -1
} ipv6_mode_t;

typedef struct {
    int original_accept_ra;
    int original_autoconf;
    int original_forwarding;
    char backup_file[PATH_MAX];
    int backup_exists;
} ipv6_backup_t;

int ipv6_manager_init(atp_config_t *cfg);
int ipv6_manager_set_mode(atp_config_t *cfg, int mode);
int ipv6_manager_backup(atp_config_t *cfg, ipv6_backup_t *backup);
int ipv6_manager_restore(atp_config_t *cfg, ipv6_backup_t *backup);
int ipv6_manager_disable_all(atp_config_t *cfg);
int ipv6_manager_enable_all(atp_config_t *cfg);
int ipv6_manager_is_disabled(void);

#endif
