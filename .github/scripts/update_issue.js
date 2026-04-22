module.exports = async ({ github, context, core }) => {
  const { SIZE, VER, COMMIT_MSG, TARGET_ISSUE } = process.env;
  const now = new Date();
  
  // 核心：将 GitHub Actions 默认的 UTC 时间转换为北京时间 (UTC+8)
  const localTime = new Date(now.getTime() + 8 * 3600 * 1000);
  const fmt = (v) => v.toString().padStart(2, '0');
  
  // 严格复刻 #316 紧凑格式: YYMMDD HH:mm
  const ts = `${localTime.getFullYear().toString().slice(-2)}${fmt(localTime.getMonth()+1)}${fmt(localTime.getDate())} ${fmt(localTime.getHours())}:${fmt(localTime.getMinutes())}`;
  
  // 构造交互链接
  const runUrl = `https://github.com/${context.repo.owner}/${context.repo.repo}/actions/runs/${context.runId}`;
  const commitSha = context.sha.substring(0, 7);
  const commitLink = `https://github.com/${context.repo.owner}/${context.repo.repo}/commit/${context.sha}`;

  /**
   * 交互界面设计规格：
   * 1. Summary (摘要): 🟢 [#编号] 时间戳 📦 Size: 大小
   * 2. Details (详情): 使用严谨的 "ATPd Version" 标题
   * 3. 结构: HTML 标签与 Markdown 内容之间保留空行，确保超链接在所有平台正确渲染
   */
  const newEntry = `
<details>
<summary>🟢 <b>[#${context.runNumber}](${runUrl})</b> &nbsp;&nbsp; ${ts} &nbsp;&nbsp; 📦 <b>Size:</b> ${SIZE}</summary>

### Build Delivery (Decoupled)
* 📥 **[Download Build Artifact](${runUrl})**
* 📝 **ATPd Version:** \`${VER}\`
* 🔗 **Source:** Commit [${commitSha}](${commitLink})
* 🕒 **Build Time:** \`${ts}\`

---
</details>
`.trim();

  try {
    // 1. 获取 Issue 当前内容
    const { data: issue } = await github.rest.issues.get({
      owner: context.repo.owner,
      repo: context.repo.repo,
      issue_number: parseInt(TARGET_ISSUE)
    });

    // 2. 将新记录插入 Issue 顶部，实现时间轴倒序
    await github.rest.issues.update({
      owner: context.repo.owner,
      repo: context.repo.repo,
      issue_number: parseInt(TARGET_ISSUE),
      body: newEntry + "\n\n" + (issue.body || "")
    });
    
    console.log(`Successfully updated Issue #${TARGET_ISSUE} with Build #${context.runNumber}`);
  } catch (e) {
    // 异常处理：确保 CI 即使更新看板失败也能给出明确错误
    core.setFailed(`[UI Update Error]: ${e.message}`);
  }
};
