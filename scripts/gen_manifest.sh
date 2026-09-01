#!/bin/bash
# ATPd Manifest Generator - Architect Edition

OUTPUT="manifest.md"

echo "## 📂 ATPd 项目架构快照" > $OUTPUT
echo "生成时间: $(date '+%Y-%m-%d %H:%M:%S')" >> $OUTPUT
echo "当前分支: **$(git rev-parse --abbrev-ref HEAD)**" >> $OUTPUT

echo -e "\n### 🏗️ 目录结构 (Source Only)" >> $OUTPUT
echo '```text' >> $OUTPUT
# 检查 tree 命令是否存在，不存在则使用 find 替代
if command -v tree >/dev/null 2>&1; then
    tree -L 2 -I "build|docs|*.md|tests|.git" src >> $OUTPUT
else
    find src -maxdepth 2 -not -path '*/.*' >> $OUTPUT
fi
echo '```' >> $OUTPUT

echo -e "\n### 📊 模块审计概览" >> $OUTPUT
echo "| 模块 (File) | 行数 | 风险点扫描 (strncpy/realloc) | 状态 |" >> $OUTPUT
echo "| :--- | :--- | :--- | :--- |" >> $OUTPUT

find src -name "*.c" -o -name "*.h" 2>/dev/null | while read file; do
    lines=$(wc -l < "$file")
    # 扫描编译器敏感项
    risks=$(grep -cE "strncpy|realloc" "$file")
    status="⏳ 待审计"
    [[ $risks -eq 0 ]] && status="🟢 形式合规"
    echo "| \`$(basename "$file")\` | $lines | $risks | $status |" >> $OUTPUT
done

echo -e "\n### 🚩 待处理任务 (TODO/FIXME)" >> $OUTPUT
grep -rnE "TODO|FIXME" src 2>/dev/null | sed 's/^/* /' >> $OUTPUT || echo "暂无标记任务" >> $OUTPUT

echo -e "\n---" >> $OUTPUT
echo "注意：此文件由脚本自动生成，用于同步至 ATPd 专家模式进行审计。" >> $OUTPUT
