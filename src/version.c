#include "version.h"
#include <stdio.h>
#include <string.h>

/* Version information structure */
typedef struct {
    const char *version;
    int major;
    int minor;
    int patch;
    const char *suffix;
    int build;
    const char *commit;
    const char *branch;
    int dirty;
    int clean;
    const char *build_date;
    const char *build_time;
    const char *compiler;
    const char *arch;
} atp_version_t;

static const atp_version_t g_version = {
    .version = ATP_VERSION_STRING,
    .major = ATP_VERSION_MAJOR,
    .minor = ATP_VERSION_MINOR,
    .patch = ATP_VERSION_PATCH,
    .suffix = ATP_VERSION_SUFFIX,
    .build = ATP_VERSION_BUILD,
    .commit = ATP_VERSION_COMMIT,
    .branch = ATP_VERSION_BRANCH,
    .dirty = ATP_VERSION_DIRTY[0] != '\0',
    .clean = ATP_VERSION_CLEAN,
    .build_date = ATP_BUILD_DATE,
    .build_time = ATP_BUILD_TIME,
    .compiler = ATP_BUILD_COMPILER,
    .arch = ATP_BUILD_ARCH
};

/* Get version string */
const char* atp_get_version(void) {
    return g_version.version;
}

/* Get full version info (with build details) */
const char* atp_get_full_version(void) {
    static char full_version[256];
    
    if (g_version.build > 0) {
        snprintf(full_version, sizeof(full_version),
                 "%s (build %d, commit %s, branch %s)",
                 g_version.version, g_version.build, g_version.commit, g_version.branch);
    } else {
        snprintf(full_version, sizeof(full_version),
                 "%s (commit %s, branch %s)",
                 g_version.version, g_version.commit, g_version.branch);
    }
    
    if (g_version.dirty) {
        strncat(full_version, " [DIRTY]", sizeof(full_version) - strlen(full_version) - 1);
    }
    
    return full_version;
}

/* Get version components */
int atp_get_version_major(void) { return g_version.major; }
int atp_get_version_minor(void) { return g_version.minor; }
int atp_get_version_patch(void) { return g_version.patch; }
const char* atp_get_version_suffix(void) { return g_version.suffix; }
int atp_get_version_build(void) { return g_version.build; }
const char* atp_get_version_commit(void) { return g_version.commit; }
const char* atp_get_version_branch(void) { return g_version.branch; }
int atp_is_dirty(void) { return g_version.dirty; }
int atp_is_clean(void) { return g_version.clean; }

/* Get build information */
const char* atp_get_build_date(void) { return g_version.build_date; }
const char* atp_get_build_time(void) { return g_version.build_time; }
const char* atp_get_compiler(void) { return g_version.compiler; }
const char* atp_get_arch(void) { return g_version.arch; }
