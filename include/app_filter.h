#ifndef ATP_APP_FILTER_H
#define ATP_APP_FILTER_H

#include "atp.h"

typedef struct {
    int uid;
    char package_name[256];
    int user_id;
} app_info_t;

int app_filter_init(atp_config_t *cfg);
int app_filter_setup(atp_config_t *cfg);
int app_filter_cleanup(atp_config_t *cfg);
int app_filter_reload(atp_config_t *cfg);
int app_filter_get_uid_by_package(const char *package_name, int user_id);
int app_filter_resolve_packages(const char *packages_list, int **uids, int *count);
void app_filter_free_uids(int *uids);

/* New: Connection-level control */
int app_filter_should_proxy(int family, int protocol,
                             uint32_t src_ip, uint16_t src_port,
                             uint32_t dst_ip, uint16_t dst_port);
int app_filter_get_connection_uid(int family, int protocol,
                                    uint32_t src_ip, uint16_t src_port,
                                    uint32_t dst_ip, uint16_t dst_port);

#endif
