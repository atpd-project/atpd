#!/bin/bash
set -e

BRANCH="${GITHUB_REF_NAME:-$(git rev-parse --abbrev-ref HEAD)}"

if [ "$BRANCH" != "staging" ]; then
    echo "Skipping auto tag: not on staging (current: $BRANCH)"
    exit 0
fi

LATEST_TAG=$(git tag -l "[0-9]*" --sort=-v:refname | head -1 || echo "")

if [ -z "$LATEST_TAG" ]; then
    NEXT_TAG="0.1-beta.1"
else
    BASE=$(echo "$LATEST_TAG" | cut -d- -f1)
    LATEST_BETA=$(git tag -l "${BASE}-beta.*" --sort=-v:refname | head -1 || echo "")
    
    if [ -z "$LATEST_BETA" ]; then
        NEXT_TAG="${BASE}-beta.1"
    else
        NUM=$(echo "$LATEST_BETA" | grep -oP 'beta\.\K\d+' || echo "0")
        NEXT_NUM=$((NUM + 1))
        NEXT_TAG="${BASE}-beta.${NEXT_NUM}"
    fi
fi

echo "Next beta tag: $NEXT_TAG"
git config user.email "ci@atpd.project"
git config user.name "ATP CI"
git tag -a "$NEXT_TAG" -m "$NEXT_TAG"
git push origin "$NEXT_TAG"
echo "Tag pushed: $NEXT_TAG"
