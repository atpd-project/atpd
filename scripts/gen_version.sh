#!/bin/sh
# ATP Version Generator
# Output: include/version.h with git-derived version

VERSION_FILE="include/version.h"
BUILD_DATE=$(date +"%Y-%m-%d %H:%M:%S")

HEAD_COMMIT=$(git rev-parse HEAD)

# Get latest v* tag that points to HEAD (lightweight or annotated)
TAG=""
for t in $(git tag -l "v*" --sort=-v:refname); do
    TAG_COMMIT=$(git rev-parse "$t^{commit}" 2>/dev/null)
    if [ "$TAG_COMMIT" = "$HEAD_COMMIT" ]; then
        TAG="$t"
        break
    fi
done

# Fallback: get latest v* tag (for non-tagged commits)
if [ -z "$TAG" ]; then
    TAG=$(git tag -l "v*" --sort=-v:refname | head -1)
fi
if [ -z "$TAG" ]; then
    TAG="v0"
fi

COMMIT=$(git rev-parse --short HEAD 2>/dev/null || echo "unknown")

# If HEAD is exactly on a tag, use tag name only
HEAD_COMMIT_FULL=$(git rev-parse HEAD)
TAG_ON_HEAD=""
for t in $(git tag -l "v*"); do
    t_commit=$(git rev-parse "$t^{commit}" 2>/dev/null)
    if [ "$t_commit" = "$HEAD_COMMIT_FULL" ]; then
        TAG_ON_HEAD="$t"
        break
    fi
done

if [ -n "$TAG_ON_HEAD" ]; then
    VERSION="$TAG_ON_HEAD"
else
    VERSION="${TAG}.${COMMIT}"
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
#define ATP_BUILD_DATE      "${BUILD_DATE}"
#define ATP_COMMIT          "${COMMIT}"

#endif /* ATP_VERSION_H */
HEADER

echo "Version: $VERSION (commit: $COMMIT, built: $BUILD_DATE)"
