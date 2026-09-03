#include "service_credentials.h"

#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int passwd_lookups;
static int group_lookups;

static struct passwd *missing_passwd(const char *name) {
    (void)name;
    passwd_lookups++;
    return NULL;
}

static struct group *missing_group(const char *name) {
    (void)name;
    group_lookups++;
    return NULL;
}

int main(void) {
    service_credentials_t credentials;

    if (service_credentials_resolve_with_lookups(
               "root", "root", 123, 456, missing_passwd, missing_group,
               &credentials) != 0) abort();
    if (credentials.uid != 0 || credentials.gid != 0) abort();
    if (credentials.init_groups) abort();
    if (passwd_lookups != 0 || group_lookups != 0) abort();

    if (service_credentials_resolve_with_lookups(
               "1234", "5678", 0, 0, missing_passwd, missing_group,
               &credentials) != 0) abort();
    if (credentials.uid != 1234 || credentials.gid != 5678) abort();
    if (credentials.init_groups) abort();

    struct passwd *named = getpwnam("nobody");
    if (!named || !named->pw_name || strcmp(named->pw_name, "root") == 0) abort();
    char named_user[64];
    snprintf(named_user, sizeof(named_user), "%s", named->pw_name);
    uid_t named_uid = named->pw_uid;
    gid_t named_gid = named->pw_gid;
    struct group *named_group = getgrgid(named_gid);
    if (!named_group || !named_group->gr_name) abort();
    char group_name[64];
    snprintf(group_name, sizeof(group_name), "%s", named_group->gr_name);
    if (service_credentials_resolve(named_user, group_name,
                                    0, 0,
                                    &credentials) != 0) abort();
    if (credentials.uid != named_uid) abort();
    if (credentials.gid != named_gid) abort();
    if (!credentials.init_groups) abort();
    if (strcmp(credentials.user_name, named_user) != 0) abort();

    if (service_credentials_resolve_with_lookups(
               "atpd-user-that-must-not-exist", "0", 0, 0,
               missing_passwd, missing_group, &credentials) == 0) abort();
    if (passwd_lookups != 1) abort();

    puts("Service credential tests passed");
    return 0;
}
