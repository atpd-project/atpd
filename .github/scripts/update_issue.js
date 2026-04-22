module.exports = async ({ github, context, core }) => {
  const { SIZE, VER, COMMIT_MSG, TARGET_ISSUE } = process.env;
  const now = new Date();
  
  // 转换至北京时间 (UTC+8)
  const localTime = new Date(now.getTime() + 8 * 3600 * 1000);
  const fmt = (v) => v.toString().padStart(2, '0');
  
  // YYMMDD HH:mm
  const ts = `${localTime.getFullYear().toString().slice(-2)}${fmt(localTime.getMonth()+1)}${fmt(localTime.getDate())} ${fmt(localTime.getHours())}:${fmt(localTime.getMinutes())}`;
  const runUrl = `https://github.com/${context.repo.owner}/${context.repo.repo}/actions/runs/${context.runId}`;
  
  const commitSha = context.sha.substring(0, 7);
  const commitLink = `https://github.com/${context.repo.owner}/${context.repo.repo}/commit/${context.sha}`;

  /**
   * 🛠️ 交互界面极致修复：
   * 1. 修正 href 拼写（之前写成了 herf）。
   * 2. 在 <summary> 内部使用全 HTML 渲染。
   * 3. 确保详情页 Markdown 语法顶格，避免缩进导致解析成代码块。
   */
  const newEntry = `
<details>
<summary>🟢 <a href="${runUrl}"><b>[#${context.runNumber}]</b></a> &nbsp;&nbsp; ${ts} &nbsp;&nbsp; 📦 <b>Size:</b> ${SIZE}</summary>

### Build Delivery (Decoupled)

* 📥 **[Download Build Artifact](${runUrl})**
* 📝 **ATPd Version:** \`${VER}\`
* 🔗 **Source:** Commit [${commitSha}](${commitLink})
* 🕒 **Build Time:** \`${ts}\`

---
</details>`.trim();

  try {
    const { data: issue } = await github.rest.issues.get({
      owner: context.repo.owner,
      repo: context.repo.repo,
      issue_number: parseInt(TARGET_ISSUE)
    });

    // 顶部插入新记录
    await github.rest.issues.update({
      owner: context.repo.owner,
      repo: context.repo.repo,
      issue_number: parseInt(TARGET_ISSUE),
      body: newEntry + "\n\n" + (issue.body || "")
    });
  } catch (e) {
    core.setFailed(`[UI Update Error]: ${e.message}`);
  }
};
