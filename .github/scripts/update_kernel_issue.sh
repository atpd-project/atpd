#!/bin/bash
set -e

ISSUE_NUM="${TARGET_ISSUE:-14}"
TAG="${TAG}"
S_TAG="${S_TAG}"
KSU_VER="${KSU_VER}"
KOWSU_VER="${KOWSU_VER}"
ATPD_SHA="${ATPD_SHA}"
REPO="${GITHUB_REPOSITORY:-atpd-project/atpd}"
RUN_ID="${GITHUB_RUN_ID}"
RUN_NUM="${GITHUB_RUN_NUMBER}"
BRANCH="${GITHUB_REF_NAME}"

NOW=$(date -u +"%Y-%m-%dT%H:%M:%SZ")
LOCAL_TIME=$(date -d "$NOW" +"%y%m%d %H:%M")

RUN_URL="https://github.com/${REPO}/actions/runs/${RUN_ID}"
RELEASE_URL="https://github.com/${REPO}/releases/tag/${TAG}"

NEW_ENTRY="
<details>
<summary>🔧 [<b><a href=\"${RUN_URL}\">#${RUN_NUM}</a></b>] &nbsp;&nbsp; ${LOCAL_TIME} &nbsp;&nbsp; 📱 zuma, gs201, zumapro</summary>

* 🏷️ **Sultan:** \`${S_TAG}\`
* 🔧 **xxKSU:** \`r${KSU_VER}\`
* 📦 **KowSU:** \`${KOWSU_VER}\`
* 🔗 **ATPd Compat:** \`${ATPD_SHA}\`
* 📥 **[Download Release](${RELEASE_URL})**
* 🕒 **Build Time:** \`${LOCAL_TIME}\`

---

</details>
"

ISSUE_BODY=$(gh issue view "$ISSUE_NUM" --json body --jq '.body')
FINAL_BODY="${NEW_ENTRY}"$'\n'$'\n'$'\n'"${ISSUE_BODY}"

echo "$FINAL_BODY" | gh issue edit "$ISSUE_NUM" --body-file -

echo "Issue #${ISSUE_NUM} updated with build #${RUN_NUM}"
