#!/bin/bash
# ATPD Rust CI Notification Service: Telegram + build-history dashboard issue.
set -e

CHANNEL="${1:-all}"
REPO="${GITHUB_REPOSITORY:-atpd-project/atpd}"
ISSUE="${DASHBOARD_ISSUE:-131}"
RUN_NUM="${GITHUB_RUN_NUMBER:-0}"
export TZ='Asia/Tokyo'
TIMESTAMP=$(date +"%y%m%d %H:%M")

escape_md() {
    local str="$1"
    str="${str//\\/\\\\}"
    str="${str//_/\\_}"
    str="${str//\*/\\*}"
    str="${str//\[/\\[}"
    str="${str//\]/\\]}"
    str="${str//(/\\\(}"
    str="${str//)/\\\)}"
    str="${str//~/\\~}"
    str="${str//\`/\\\`}"
    str="${str//>/\\>}"
    str="${str//#/\\#}"
    str="${str//+/\\+}"
    str="${str//-/\\-}"
    str="${str//=/\\=}"
    str="${str//|/\\|}"
    str="${str//\{/\\\{}"
    str="${str//\}/\\\}}"
    str="${str//./\\.}"
    str="${str//!/\\!}"
    echo "$str"
}

escape_url() {
    local str="$1"
    str="${str//\\/\\\\}"
    str="${str//)/\\)}"
    echo "$str"
}

resolve_assets() {
    local tag="${TAG_NAME:-${GITHUB_REF_NAME:-}}"
    BIONIC_FILE="${BIONIC_FILENAME:-}"
    BIONIC_URL="${BIONIC_DOWNLOAD_URL:-}"

    if [[ "$tag" == v* ]]; then
        [ -z "$BIONIC_FILE" ] && BIONIC_FILE="atpd-${tag#v}-android-arm64.tar.gz"
        [ -z "$BIONIC_URL" ] && BIONIC_URL="https://github.com/${REPO}/releases/download/${tag}/${BIONIC_FILE}"
    fi
}

notify_telegram() {
    local token="${TELEGRAM_BOT_TOKEN}"
    local chat_id="${TELEGRAM_CHAT_ID}"
    [ -z "$token" ] && { echo "  Telegram: TELEGRAM_BOT_TOKEN not set"; return 0; }
    [ -z "$chat_id" ] && { echo "  Telegram: TELEGRAM_CHAT_ID not set"; return 0; }

    resolve_assets

    local tag="${TAG_NAME:-${GITHUB_REF_NAME:-}}"
    local repo_esc=$(escape_md "$REPO")
    local native_esc=$(escape_md "${NATIVE_SIZE:-N/A}")
    local bionic_esc=$(escape_md "${BIONIC_SIZE:-N/A}")

    local msg=""
    if [[ "$tag" == v* ]]; then
        local bionic_file_esc=$(escape_md "$BIONIC_FILE")
        local bionic_url_esc=$(escape_url "$BIONIC_URL")

        msg="📋 *Latest ATPd Rust Release*%0A"
        msg+="🟢 \#${RUN_NUM} \| ${TIMESTAMP}%0A"
        msg+="🤖 [${bionic_file_esc}](${bionic_url_esc}) \(${bionic_esc}\)%0A"
        msg+="🔗 https://github\.com/${repo_esc}/issues/${ISSUE}"
    else
        msg="📋 *Latest ATPd Rust Build*%0A"
        msg+="🟢 \#${RUN_NUM} \| ${TIMESTAMP}%0A"
        msg+="📦 Native \(CI\-only\): ${native_esc}%0A"
        msg+="🤖 Bionic: ${bionic_esc}%0A"
        msg+="🔗 https://github\.com/${repo_esc}/issues/${ISSUE}"
    fi

    curl -s -X POST "https://api.telegram.org/bot${token}/sendMessage" \
        -d "chat_id=${chat_id}" \
        -d "text=${msg}" \
        -d "parse_mode=MarkdownV2" \
        -d "disable_web_page_preview=true"
    echo "  Telegram: sent"
}

notify_issue() {
    VER="${ATP_VERSION:-unknown}"
    NATIVE="${NATIVE_SIZE:-N/A}"
    RUNTIME="${ATPD_VERSION_STRING:-atpd ${VER}}"
    NATIVE_COMPILER="${NATIVE_COMPILER_VER:-N/A}"
    BIONIC="${BIONIC_SIZE:-N/A}"
    BIONIC_COMPILER="${BIONIC_COMPILER_VER:-N/A}"

    resolve_assets

    ISSUE_BODY=$(gh issue view "$ISSUE" --json body --jq '.body')
    RUN_URL="https://github.com/${REPO}/actions/runs/${GITHUB_RUN_ID}"
    BRANCH="${GITHUB_REF_NAME:-unknown}"
    COMMIT_SHA="${GITHUB_SHA:-unknown}"
    COMMIT_SHORT="${COMMIT_SHA:0:7}"
    COMMIT_LINK="https://github.com/${REPO}/commit/${COMMIT_SHA}"
    BRANCH_LINK="https://github.com/${REPO}/tree/${BRANCH}"
    RAW_MSG="${COMMIT_TITLE:-${COMMIT_MSG:-No commit message}}"
    DISPLAY_MSG="${RAW_MSG:0:42}"
    [ ${#RAW_MSG} -gt 42 ] && DISPLAY_MSG="${DISPLAY_MSG}..."

    SUMMARY_SIZE="${BIONIC} (Bionic) / ${NATIVE} (Native, CI-only)"

    TS=$(TZ='Asia/Tokyo' date +"%y%m%d %H:%M")

    DOWNLOAD_LINK="${RUN_URL}"
    DOWNLOAD_LABEL="Download Build Artifact"
    NATIVE_LINE="* ⚙️ **Native (CI-only):** \`${NATIVE_COMPILER}\` | 📦 \`${NATIVE}\`"
    BIONIC_LINE="* 🤖 **Bionic:** \`${BIONIC_COMPILER}\` | 📦 \`${BIONIC}\`"
    if [[ "$BRANCH" == v* ]]; then
        DOWNLOAD_LINK="https://github.com/${REPO}/releases/tag/${BRANCH}"
        DOWNLOAD_LABEL="Download Release Assets"
        BIONIC_LINE="* 🤖 **Bionic:** \`${BIONIC_COMPILER}\` | 📦 [${BIONIC_FILE}](${BIONIC_URL}) (\`${BIONIC}\`)"
    fi

    NEW_ENTRY="<details>
<summary>🟢 [<b><a href=\"${RUN_URL}\">#${RUN_NUM}</a></b>] &nbsp;&nbsp; ${TS} &nbsp;&nbsp; 📦 <b>Size:</b> ${SUMMARY_SIZE}</summary>

### Branch: [${BRANCH}](${BRANCH_LINK})

* 📥 **[${DOWNLOAD_LABEL}](${DOWNLOAD_LINK})**
* 📝 **CI Version:** \`${VER}\`
* 🔧 **ATPd Version (-v):** \`${RUNTIME}\`
${BIONIC_LINE}
${NATIVE_LINE}
* 💬 **Message:** \`${DISPLAY_MSG}\`
* 🔗 **Source:** Commit [${COMMIT_SHORT}](${COMMIT_LINK})
* 🕒 **Build Time:** \`${TS}\`

---

</details>"

    # Retain at most 20 previous entries to prevent hitting GitHub's 65KB issue body limit
    CLEAN_BODY=$(echo "$ISSUE_BODY" | awk 'BEGIN{c=0} /<details>/{c++} c<=20{print}')
    FINAL_BODY="${NEW_ENTRY}

${CLEAN_BODY}"

    # Hard length safeguard (under 55KB)
    if [ ${#FINAL_BODY} -gt 55000 ]; then
        FINAL_BODY="${FINAL_BODY:0:50000}

... (older build logs trimmed)"
    fi

    if echo "$FINAL_BODY" | gh issue edit "$ISSUE" --body-file - 2>/dev/null; then
        echo "  Issue #${ISSUE}: updated (ver: $VER, runtime: $RUNTIME)"
    else
        echo "  Warning: Full history update failed, resetting to latest 5 entries..."
        FALLBACK_BODY="${NEW_ENTRY}"
        echo "$FALLBACK_BODY" | gh issue edit "$ISSUE" --body-file - || echo "  Issue #${ISSUE} update skipped"
    fi
}

echo "=== ATP Notification: $CHANNEL ==="

case "$CHANNEL" in
    telegram)
        notify_telegram
        ;;
    issue)
        notify_issue
        ;;
    all)
        notify_issue
        notify_telegram
        ;;
    *)
        echo "Unknown channel: $CHANNEL"
        exit 1
        ;;
esac

echo "  Done at $TIMESTAMP"
