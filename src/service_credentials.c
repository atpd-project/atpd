#include "service_credentials.h"

#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>

static int parse_uid(const char *text, uid_t *uid) {
    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0' ||
        (unsigned long)(uid_t)value != value) {
        errno = EINVAL;
        return -1;
    }
    *uid = (uid_t)value;
    return 0;
}

static int parse_gid(const char *text, gid_t *gid) {
    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0' ||
        (unsigned long)(gid_t)value != value) {
        errno = EINVAL;
        return -1;
    }
    *gid = (gid_t)value;
    return 0;
}

int service_credentials_resolve_with_lookups(
    const char *user, const char *group, uid_t default_uid, gid_t default_gid,
    service_passwd_lookup_fn passwd_lookup,
    service_group_lookup_fn group_lookup,
    service_credentials_t *credentials) {
    if (!passwd_lookup || !group_lookup || !credentials) {
        errno = EINVAL;
        return -1;
    }

    memset(credentials, 0, sizeof(*credentials));
    credentials->uid = default_uid;
    credentials->gid = default_gid;

    if (user && user[0]) {
        if (strcmp(user, "root") == 0) {
            credentials->uid = 0;
            credentials->gid = 0;
        } else if (user[0] >= '0' && user[0] <= '9') {
            if (parse_uid(user, &credentials->uid) != 0) return -1;
        } else {
            struct passwd *pwd = passwd_lookup(user);
            if (!pwd) {
                errno = ENOENT;
                return -1;
            }
            credentials->uid = pwd->pw_uid;
            credentials->gid = pwd->pw_gid;
            credentials->init_groups = true;
            if (strlen(pwd->pw_name) >= sizeof(credentials->user_name)) {
                errno = ENAMETOOLONG;
                return -1;
            }
            strcpy(credentials->user_name, pwd->pw_name);
        }
    }

    if (group && group[0]) {
        if (strcmp(group, "root") == 0) {
            credentials->gid = 0;
        } else if (group[0] >= '0' && group[0] <= '9') {
            if (parse_gid(group, &credentials->gid) != 0) return -1;
        } else {
            struct group *grp = group_lookup(group);
            if (!grp) {
                errno = ENOENT;
                return -1;
            }
            credentials->gid = grp->gr_gid;
        }
    }

    return 0;
}

int service_credentials_resolve(const char *user, const char *group,
                                uid_t default_uid, gid_t default_gid,
                                service_credentials_t *credentials) {
    return service_credentials_resolve_with_lookups(
        user, group, default_uid, default_gid, getpwnam, getgrnam, credentials);
}
