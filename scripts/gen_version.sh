#!/bin/sh
# ATP Version Generator
# Output: include/version.h with git-derived version

VERSION_FILE="include/version.h"

TAG=$(git tag -l "v*" --sort=-v:refname 2>/dev/null | head -1)
if [ -z "$TAG" ]; then
    TAG="v0"
fi

COMMIT=$(git rev-parse --short HEAD 2>/dev/null || echo "unknown")

if git describe --tags --exact-match --match="v*" >/dev/null 2>&1; then
    VERSION="$TAG"
else
    VERSION="${TAG}.${COMMIT}"
fi

if ! git diff-index --quiet HEAD -- 2>/dev/null; then
    VERSION="${VERSION}-dirty"
fi

cat > "$VERSION_FILE" << HEADER
/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * Auto-generated version header - DO NOT EDIT
 */

#ifndef ATP_VERSION_H
#define ATP_VERSION_H

#define ATP_VERSION_STRING  "${VERSION}"
#define ATP_VERSION         "${VERSION}"
#define ATP_COMMIT          "${COMMIT}"

#endif /* ATP_VERSION_H */
HEADER

echo "Version: $VERSION (commit: $COMMIT)"
