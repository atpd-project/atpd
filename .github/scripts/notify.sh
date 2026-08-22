#!/bin/bash
# ATP CI Notification Service
set -e

CHANNEL="${1:-all}"
REPO="${GITHUB_REPOSITORY:-atpd-project/atpd}"
RUN_NUM="${GITHUB_RUN_NUMBER:-0}"
export TZ='Asia/Shanghai'
TIMESTAMP=$(date +"%y%m%d %H:%M")

escape_md() {
    local str="$1"
    str="${str//_/\\_}"
    str="${str//\*/\\*}"
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

notify_telegram() {
    local token="${TELEGRAM_BOT_TOKEN}"
    local chat_id="${TELEGRAM_CHAT_ID}"
    [ -z "$token" ] && { echo "  Telegram: TELEGRAM_BOT_TOKEN not set"; return 0; }
    [ -z "$chat_id" ] && { echo "  Telegram: TELEGRAM_CHAT_ID not set"; return 0; }

    local repo_esc=$(escape_md "$REPO")
    local clang_esc=$(escape_md "${CLANG_SIZE:-N/A}")
    local zig_esc=$(escape_md "${ZIG_SIZE:-N/A}")

    local msg="📋 *Latest ATPd Build*%0A"
    msg+="🟢 \#${RUN_NUM} \| ${TIMESTAMP}%0A"
    msg+="📦 Clang: ${clang_esc} \| Zig: ${zig_esc}%0A"
    msg+="🔗 https://github\.com/${repo_esc}/issues/6"

    curl -s -X POST "https://api.telegram.org/bot${token}/sendMessage" \
        -d "chat_id=${chat_id}" \
        -d "text=${msg}" \
        -d "parse_mode=MarkdownV2" \
        -d "disable_web_page_preview=true"
    echo "  Telegram: sent"
}

notify_issue() {
    if [ -f include/version.h ]; then
        VER=$(grep -oP 'ATP_VERSION_STRING\s+"\K[^"]+' include/version.h || echo "unknown")
    else
        VER="${ATP_VERSION:-unknown}"
    fi

    CLANG="${CLANG_SIZE:-N/A}"
    ZIG="${ZIG_SIZE:-N/A}"
    COMPILER="${COMPILER_VER:-N/A}"
    RUNTIME="${ATPD_VERSION_STRING:-atpd ${VER}}"
    ZIG_COMPILER="${ZIG_COMPILER_VER:-N/A}"

    ISSUE_BODY=$(gh issue view 6 --json body --jq '.body')
    RUN_URL="https://github.com/${REPO}/actions/runs/${GITHUB_RUN_ID}"
    BRANCH="${GITHUB_REF_NAME:-unknown}"
    COMMIT_SHA="${GITHUB_SHA:-unknown}"
    COMMIT_SHORT="${COMMIT_SHA:0:7}"
    COMMIT_LINK="https://github.com/${REPO}/commit/${COMMIT_SHA}"
    BRANCH_LINK="https://github.com/${REPO}/tree/${BRANCH}"
    RAW_MSG="${COMMIT_TITLE:-${COMMIT_MSG:-No commit message}}"
    DISPLAY_MSG="${RAW_MSG:0:42}"
    [ ${#RAW_MSG} -gt 42 ] && DISPLAY_MSG="${DISPLAY_MSG}..."

    SUMMARY_SIZE="${CLANG} (Clang 21) / ${ZIG} (Zig CC)"

    TS=$(TZ='Asia/Shanghai' date +"%y%m%d %H:%M")

    NEW_ENTRY="<details>
<summary>🟢 [<b><a href=\"${RUN_URL}\">#${RUN_NUM}</a></b>] &nbsp;&nbsp; ${TS} &nbsp;&nbsp; 📦 <b>Size:</b> ${SUMMARY_SIZE}</summary>

### Branch: [${BRANCH}](${BRANCH_LINK})

* 📥 **[Download Build Artifact](${RUN_URL})**
* 📝 **CI Version:** \`${VER}\`
* 🔧 **ATPd Version (-v):** \`${RUNTIME}\`
* ⚙️ **Clang 21:** \`${COMPILER}\` | 📦 \`${CLANG}\`
* ⚙️ **Zig CC:** \`${ZIG_COMPILER}\` | 📦 \`${ZIG}\`
* 💬 **Message:** \`${DISPLAY_MSG}\`
* 🔗 **Source:** Commit [${COMMIT_SHORT}](${COMMIT_LINK})
* 🕒 **Build Time:** \`${TS}\`

---

</details>"

    FINAL_BODY="${NEW_ENTRY}

${ISSUE_BODY}"

    echo "$FINAL_BODY" | gh issue edit 6 --body-file -
    echo "  Issue #6: updated (ver: $VER, runtime: $RUNTIME)"
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
