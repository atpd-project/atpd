#!/bin/bash
#
# Generate version.h with dynamic version string
# - Tag build: uses tag name (e.g., v1.2.0 -> 1.2.0)
# - Dev build: uses base version from version.h + commit hash
# - Dirty workspace: adds -dirty suffix
#

set -e

DEFAULT_VERSION="0.0.1-dev"
MAJOR=0
MINOR=0
PATCH=1

if [ -f include/version.h ]; then
    BASE_VERSION=$(grep ATP_VERSION_STRING include/version.h | head -1 | cut -d'"' -f2)
    MAJOR=$(grep ATP_VERSION_MAJOR include/version.h | awk '{print $3}')
    MINOR=$(grep ATP_VERSION_MINOR include/version.h | awk '{print $3}')
    PATCH=$(grep ATP_VERSION_PATCH include/version.h | awk '{print $3}')
    BASE_VERSION="${BASE_VERSION%-dev}"
else
    BASE_VERSION="$DEFAULT_VERSION"
    BASE_VERSION="${BASE_VERSION%-dev}"
fi

if git rev-parse --git-dir >/dev/null 2>&1; then
    if git describe --tags --exact-match 2>/dev/null; then
        TAG=$(git describe --tags --exact-match)
        VERSION="${TAG#v}"
    else
        COMMIT=$(git rev-parse --short HEAD)
        VERSION="${BASE_VERSION}-${COMMIT}"
        if ! git diff --quiet 2>/dev/null; then
            VERSION="${VERSION}-dirty"
        fi
    fi
else
    VERSION="${BASE_VERSION}-unknown"
fi

cat > include/version.h << EOF
#ifndef ATP_VERSION_H
#define ATP_VERSION_H

#define ATP_VERSION_MAJOR     $MAJOR
#define ATP_VERSION_MINOR     $MINOR
#define ATP_VERSION_PATCH     $PATCH
#define ATP_VERSION_STRING    "$VERSION"

#endif
EOF

echo "Generated version.h with version $VERSION"
