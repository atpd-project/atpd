#!/bin/bash
# ATP CI Notification Service
# Usage: notify.sh <telegram|issue|all>
#
# Required env vars:
#   GITHUB_REPOSITORY, GITHUB_RUN_NUMBER, GH_TOKEN
#   TELEGRAM_BOT_TOKEN, TELEGRAM_CHAT_ID (for telegram)
#   ZIG_SIZE, CLANG_SIZE, ATP_VERSION, RUNTIME_VER, COMPILER_VER (from build)
set -e

CHANNEL="${1:-all}"
REPO="${GITHUB_REPOSITORY:-atpd-project/atpd}"
RUN_NUM="${GITHUB_RUN_NUMBER:-0}"
export TZ='Asia/Shanghai'
TIMESTAMP=$(date +"%y%m%d %H:%M")

# ── MarkdownV2 转义 ─────────────────────────────
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

# ── Telegram 通知 ────────────────────────────────
notify_telegram() {
    local token="${TELEGRAM_BOT_TOKEN}"
    local chat_id="${TELEGRAM_CHAT_ID}"

    [ -z "$token" ] && { echo "TELEGRAM_BOT_TOKEN not set"; return 0; }
    [ -z "$chat_id" ] && { echo "TELEGRAM_CHAT_ID not set"; return 0; }

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

# ── Issue #6 更新 ────────────────────────────────
notify_issue() {
    local script=".github/scripts/update_issue.js"
    if [ -f "$script" ]; then
        node "$script"
        echo "  Issue #6: updated"
    else
        echo "  Issue #6: script not found, skipped"
    fi
}

# ── 主入口 ───────────────────────────────────────
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
        echo "Usage: notify.sh <telegram|issue|all>"
        exit 1
        ;;
esac

echo "  Done at $TIMESTAMP"
