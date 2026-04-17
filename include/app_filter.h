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
int app_filter_get_uid_by_package(const char *package_name, int user_id);
int app_filter_resolve_packages(const char *packages_list, int **uids, int *count);
void app_filter_free_uids(int *uids);

#endif
