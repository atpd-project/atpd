/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2025 ATP Project
 *
 * Version information
 */

#include "version.h"
#include "atp.h"
#include <string.h>
#include <stdlib.h>

const char* atp_get_version(void) {
    return ATP_VERSION_STRING;
}

const char* atp_get_full_version(void) {
    return ATP_VERSION_STRING;
}

int atp_get_version_major(void) {
    /* Parse from v0.abc1234 format */
    const char *v = ATP_VERSION_STRING;
    if (v[0] == 'v') {
        return atoi(v + 1);
    }
    return 0;
}

int atp_get_version_minor(void) {
    return 0;  /* Not used in simplified version scheme */
}

int atp_get_version_patch(void) {
    return 0;  /* Not used in simplified version scheme */
}
