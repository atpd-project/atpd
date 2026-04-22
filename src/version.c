/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * Version information
 */

#include "version.h"
#include "atp.h"

const char* atp_version_string(void) {
    return ATP_VERSION_STRING;
}

const char* atp_build_date(void) {
    return ATP_BUILD_DATE;
}
