#ifndef ATP_VERSION_H
#define ATP_VERSION_H

#include <stdbool.h>

const char *atp_get_version(void);
const char *atp_get_full_version(void);
const char *atp_get_commit(void);
bool atp_build_is_dirty(void);

#endif
