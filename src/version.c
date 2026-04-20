/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2025 ATP Project
 *
 * Version information
 */

#include "version.h"
#include <string.h>

const char* atp_get_version(void) {
    return ATP_VERSION_STRING;
}

const char* atp_get_full_version(void) {
    return ATP_VERSION_STRING;
}

int atp_get_version_major(void) {
    return ATP_VERSION_MAJOR;
}

int atp_get_version_minor(void) {
    return ATP_VERSION_MINOR;
}

int atp_get_version_patch(void) {
    return ATP_VERSION_PATCH;
}
