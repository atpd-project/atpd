#ifndef ATP_SERVICE_CREDENTIALS_H
#define ATP_SERVICE_CREDENTIALS_H

#include <stdbool.h>
#include <grp.h>
#include <pwd.h>
#include <sys/types.h>

typedef struct {
    uid_t uid;
    gid_t gid;
    bool init_groups;
    char user_name[64];
} service_credentials_t;

typedef struct passwd *(*service_passwd_lookup_fn)(const char *name);
typedef struct group *(*service_group_lookup_fn)(const char *name);

int service_credentials_resolve(const char *user, const char *group,
                                uid_t default_uid, gid_t default_gid,
                                service_credentials_t *credentials);
int service_credentials_resolve_with_lookups(
    const char *user, const char *group, uid_t default_uid, gid_t default_gid,
    service_passwd_lookup_fn passwd_lookup,
    service_group_lookup_fn group_lookup,
    service_credentials_t *credentials);

#endif
