#!/bin/bash
set -e

TOKEN="${TELEGRAM_BOT_TOKEN}"
CHAT_ID="${TELEGRAM_CHAT_ID}"
REPO="${GITHUB_REPOSITORY:-atpd-project/atpd}"
ZIG_SIZE="${ZIG_SIZE:-N/A}"
RUN_NUM="${GITHUB_RUN_NUMBER:-0}"

ISSUE_BODY=$(gh issue view 6 --json body --jq '.body')
SIZE=$(echo "$ISSUE_BODY" | grep -oP '(?<=📦 <b>Size:</b> )[^<]*' | head -1)

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

SIZE_ESC=$(escape_md "${SIZE:-N/A}")
ZIG_SIZE_ESC=$(escape_md "${ZIG_SIZE}")

MESSAGE="📋 *Latest ATPd Build*%0A"
MESSAGE+="🟢 \#${RUN_NUM} \| ${TIMESTAMP}%0A"

if [ "${ZIG_SIZE}" != "N/A" ] && [ -n "${ZIG_SIZE}" ]; then
    MESSAGE+="📦 Clang: ${SIZE_ESC} \| Zig: ${ZIG_SIZE_ESC}%0A"
else
    MESSAGE+="📦 Clang: ${SIZE_ESC}%0A"
fi

MESSAGE+="🔗 https://github\.com/${REPO//./\\.}/issues/6"

curl -s -X POST "https://api.telegram.org/bot${TOKEN}/sendMessage" \
    -d "chat_id=${CHAT_ID}" \
    -d "text=${MESSAGE}" \
    -d "parse_mode=MarkdownV2" \
    -d "disable_web_page_preview=true"

echo "Notification sent: Build #${RUN_NUM} at ${TIMESTAMP}"
