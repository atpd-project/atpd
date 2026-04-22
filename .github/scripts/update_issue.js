module.exports = async ({ github, context, core }) => {
    const { SIZE, VER, COMMIT_MSG, TARGET_ISSUE } = process.env;
    const now = new Date();
    // 转换为北京时间 (UTC+8)
    const localTime = new Date(now.getTime() + 8 * 3600 * 1000);
    const fmt = (v) => v.toString().padStart(2, '0');
    
    // 严格匹配 #316 的紧凑时间格式: YYMMDD HH:mm
    const ts = `${localTime.getFullYear().toString().slice(-2)}${fmt(localTime.getMonth()+1)}${fmt(localTime.getDate())} ${fmt(localTime.getHours())}:${fmt(localTime.getMinutes())}`;
    const runUrl = `https://github.com/${context.repo.owner}/${context.repo.repo}/actions/runs/${context.runId}`;
    
    const commitLine = COMMIT_MSG.split('\n')[0];
    const branchName = context.ref.replace('refs/heads/', '');
    
    // 完全复刻 #316 的显示逻辑
    const newEntry = `
<details>
<summary>🟢 <b>[#${context.runNumber}](${runUrl})</b> 📦 <b>Size:</b> ${SIZE} 🏷️ <b>${VER}</b> <b>[atpd](${runUrl})</b></summary>
<br/>

- **Commit:** \`${commitLine}\`
- **Branch:** \`${branchName}\`
- **Time:** \`${ts}\`
- **Logs:** [View Actions](${runUrl})

---
</details>
`.trim();
    
    try {
        const { data: issue } = await github.rest.issues.get({
            owner: context.repo.owner,
            repo: context.repo.repo,
            issue_number: parseInt(TARGET_ISSUE)
        });
        
        await github.rest.issues.update({
            owner: context.repo.owner,
            repo: context.repo.repo,
            issue_number: parseInt(TARGET_ISSUE),
            body: newEntry + "\n\n" + (issue.body || "")
        });
    } catch (e) {
        core.setFailed(`Failed to update Issue #6: ${e.message}`);
    }
}
