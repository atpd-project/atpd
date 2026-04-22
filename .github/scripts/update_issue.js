module.exports = async ({ github, context, core }) => {
  const { SIZE, VER, COMMIT_MSG, TARGET_ISSUE } = process.env;
  const now = new Date();
  
  // 转换为北京时间 (UTC+8)
  const localTime = new Date(now.getTime() + 8 * 3600 * 1000);
  const fmt = (v) => v.toString().padStart(2, '0');
  
  // 严格还原截图时间格式: YYMMDD HH:mm
  const ts = `${localTime.getFullYear().toString().slice(-2)}${fmt(localTime.getMonth()+1)}${fmt(localTime.getDate())} ${fmt(localTime.getHours())}:${fmt(localTime.getMinutes())}`;
  const runUrl = `https://github.com/${context.repo.owner}/${context.repo.repo}/actions/runs/${context.runId}`;
  
  const commitSha = context.sha.substring(0, 7);
  const commitLink = `https://github.com/${context.repo.owner}/${context.repo.repo}/commit/${context.sha}`;

  // 严格匹配 #316 截图布局：
  // 🟢 [#编号](链接) 时间戳 📦 Size: 大小
  const newEntry = `
<details>
<summary>🟢 <b>[#${context.runNumber}](${runUrl})</b> &nbsp; ${ts} &nbsp; 📦 <b>Size:</b> ${SIZE}</summary>

### Build Delivery (Decoupled)
* 📥 **[Download Build Artifact](${runUrl})</b>
* 📝 **Runtime Version (-v):** \`${VER}\`
* 🔗 **Source:** Commit [${commitSha}](${commitLink})
* 🕒 **Build Time:** \`${ts}\`

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
    core.setFailed(`Failed to update Issue #${TARGET_ISSUE}: ${e.message}`);
  }
};
