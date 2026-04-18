#include "version.h"
#include <stdio.h>

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

/* Print full version banner */
void atp_print_version_banner(void) {
    printf("\033[1;36m"
    "    ___  __________  ____ \n"
    "   /   |/_  __/ __ \\/ __ \\\n"
    "  / /| | / / / /_/ / / / /\n"
    " / ___ |/ / / ____/ /_/ / \n"
    "/_/  |_/_/ /_/    /_____/  v%s\033[0m\n", 
    atp_get_version());
    
    printf("--------------------------------------------\n");
    printf(" Version:   %s\n", atp_get_full_version());
    printf(" Build:     %s | %s\n", atp_get_build_date(), atp_get_build_time());
    printf(" Compiler:  %s\n", atp_get_compiler());
    printf(" Arch:      %s\n", atp_get_arch());
    
    if (atp_is_dirty()) {
        printf("\033[1;33m Warning:  Uncommitted changes present\033[0m\n");
    }
    
    printf("--------------------------------------------\n\n");
}

/* Check if version matches a pattern (e.g., "0.0.1-alpha*") */
int atp_version_matches(const char *pattern) {
    /* Simple pattern matching - can be extended */
    const char *v = atp_get_version();
    const char *p = pattern;
    
    while (*p && *v) {
        if (*p == '*') {
            return 1;
        }
        if (*p != *v) {
            return 0;
        }
        p++;
        v++;
    }
    
    return (*p == '*' || (*p == '\0' && *v == '\0'));
}
