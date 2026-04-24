#!/bin/bash
# ATPd Manifest Generator - Architect Edition

OUTPUT="manifest.md"

echo "## 📂 ATPd 项目架构快照" > $OUTPUT
echo "生成时间: $(date '+%Y-%m-%d %H:%M:%S')" >> $OUTPUT
echo "当前分支: **$(git rev-parse --abbrev-ref HEAD)**" >> $OUTPUT

echo -e "\n### 🏗️ 目录结构 (Source Only)" >> $OUTPUT
echo '```text' >> $OUTPUT
tree -L 2 -I "build|docs|*.md|tests" src >> $OUTPUT
echo '```' >> $OUTPUT

echo -e "\n### 📊 模块审计概览" >> $OUTPUT
echo "| 模块 (File) | 行数 | 风险点扫描 (strncpy/malloc) | 状态 |" >> $OUTPUT
echo "| :--- | :--- | :--- | :--- |" >> $OUTPUT

find src -name "*.c" -o -name "*.h" | while read file; do
    lines=$(wc -l < "$file")
    # 扫描 GCC 15 敏感项：strncpy 或不安全的 realloc
    risks=$(grep -cE "strncpy|realloc" "$file")
    status="⏳ 待审计"
    [[ $risks -eq 0 ]] && status="🟢 形式合规"
    echo "| \`$(basename "$file")\` | $lines | $risks | $status |" >> $OUTPUT
done

echo -e "\n### 🚩 待处理任务 (TODO/FIXME)" >> $OUTPUT
grep -rnE "TODO|FIXME" src | sed 's/^/* /' >> $OUTPUT || echo "暂无标记任务" >> $OUTPUT

echo -e "\n---" >> $OUTPUT
echo "注意：此文件由脚本自动生成，用于同步至 ATPd 专家模式进行审计。" >> $OUTPUT
