#!/bin/bash
# Auto beta tag for staging branch
set -e

BRANCH="${GITHUB_REF_NAME:-$(git rev-parse --abbrev-ref HEAD)}"

if [ "$BRANCH" != "staging" ]; then
    echo "Skipping auto tag: not on staging (current: $BRANCH)"
    exit 0
fi

BASE_VERSION="0.1"
LATEST_BETA=$(git tag -l "v${BASE_VERSION}-beta.*" --sort=-v:refname | head -1)

if [ -z "$LATEST_BETA" ]; then
    NEXT_BETA="v${BASE_VERSION}-beta.1"
else
    LAST_NUM=$(echo "$LATEST_BETA" | grep -oP 'beta\.\K\d+')
    NEXT_NUM=$((LAST_NUM + 1))
    NEXT_BETA="v${BASE_VERSION}-beta.${NEXT_NUM}"
fi

echo "Next beta tag: $NEXT_BETA"
git tag -a "$NEXT_BETA" -m "$NEXT_BETA"
git push origin "$NEXT_BETA"
echo "Tag pushed: $NEXT_BETA"
