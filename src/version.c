#include "version.h"
#include "version_build.h"

const char *atp_get_version(void) {
    return ATP_VERSION_STRING;
}

const char *atp_get_full_version(void) {
    return ATP_VERSION_FULL;
}

const char *atp_get_commit(void) {
    return ATP_COMMIT;
}

bool atp_build_is_dirty(void) {
    return ATP_BUILD_DIRTY != 0;
}
