#!/bin/bash
set -e

TOKEN="${TELEGRAM_BOT_TOKEN}"
CHAT_ID="${TELEGRAM_CHAT_ID}"
REPO="${GITHUB_REPOSITORY:-atpd-project/atpd}"

ISSUE_BODY=$(gh issue view 6 --json body --jq '.body')
SUMMARY=$(echo "$ISSUE_BODY" | grep -oP '(?<=<summary>🟢 \[<b><a href=")[^"]*"[^>]*>[^<]*</a></b>\] &nbsp;&nbsp; [^&]*&nbsp;&nbsp; 📦 <b>Size:</b> [^<]*' | head -1)

if [ -z "$SUMMARY" ]; then
    echo "No build record found"
    exit 0
fi

RUN_NUM=$(echo "$SUMMARY" | grep -oP '#\K[0-9]+' | head -1)
TIMESTAMP=$(TZ='Asia/Shanghai' date +"%y%m%d %H:%M")
SIZE=$(echo "$SUMMARY" | grep -oP '(?<=📦 <b>Size:</b> )[^<]*' | head -1)

MESSAGE="📋 *Latest ATPd Build*%0A"
MESSAGE+="🟢 \#${RUN_NUM} \| ${TIMESTAMP} \| 📦 ${SIZE}%0A"
MESSAGE+="🔗 [View Issue \#6](https://github\.com/${REPO}/issues/6)"

curl -s -X POST "https://api.telegram.org/bot${TOKEN}/sendMessage" \
    -d "chat_id=${CHAT_ID}" \
    -d "text=${MESSAGE}" \
    -d "parse_mode=MarkdownV2"

echo "Notification sent: Build #${RUN_NUM}"
