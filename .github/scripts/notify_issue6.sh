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

MESSAGE="📋 *Latest ATPd Build*%0A"
MESSAGE+="🟢 \#${RUN_NUM} \| ${TIMESTAMP}%0A"
MESSAGE+="📦 Clang: \${SIZE:-N/A}\"

if [ "${ZIG_SIZE}" != "N/A" ] && [ -n "${ZIG_SIZE}" ]; then
    MESSAGE+=" \| Zig: \${ZIG_SIZE}\"
fi

MESSAGE+="%0A"
MESSAGE+="🔗 [View Issue \#6](https://github\.com/${REPO}/issues/6)"

curl -s -X POST "https://api.telegram.org/bot${TOKEN}/sendMessage" \
    -d "chat_id=${CHAT_ID}" \
    -d "text=${MESSAGE}" \
    -d "parse_mode=MarkdownV2"

echo "Notification sent: Build #${RUN_NUM} at ${TIMESTAMP}"

