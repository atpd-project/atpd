module.exports = async ({ github, context, core }) => {
  const { SIZE, VER, COMMIT_MSG, TARGET_ISSUE } = process.env;
  const now = new Date();
  
  // 转换至北京时间 (UTC+8)
  const localTime = new Date(now.getTime() + 8 * 3600 * 1000);
  const fmt = (v) => v.toString().padStart(2, '0');
  
  // 严格复刻紧凑格式: YYMMDD HH:mm
  const ts = `${localTime.getFullYear().toString().slice(-2)}${fmt(localTime.getMonth()+1)}${fmt(localTime.getDate())} ${fmt(localTime.getHours())}:${fmt(localTime.getMinutes())}`;
  
  // 交互链接构造
  const runUrl = `https://github.com/${context.repo.owner}/${context.repo.repo}/actions/runs/${context.runId}`;
  const commitSha = context.sha.substring(0, 7);
  const commitLink = `https://github.com/${context.repo.owner}/${context.repo.repo}/commit/${context.sha}`;

  /**
   * 交互界面设计规格 (UI Spec):
   * 1. 强制在 </summary> 后留出空行，否则 Markdown 无法解析。
   * 2. 移除所有内联 CSS 或额外标签，确保原生渲染。
   * 3. 使用标准 Markdown 列表符号。
   */
  const newEntry = [
    `<details>`,
    `<summary>🟢 <b>[#${context.runNumber}](${runUrl})</b> &nbsp;&nbsp; ${ts} &nbsp;&nbsp; 📦 <b>Size:</b> ${SIZE}</summary>`,
    '',
    `### Build Delivery (Decoupled)`,
    `* 📥 **[Download Build Artifact](${runUrl})**`,
    `* 📝 **ATPd Version:** \`${VER}\``,
    `* 🔗 **Source:** Commit [${commitSha}](${commitLink})`,
    `* 🕒 **Build Time:** \`${ts}\``,
    '',
    `---`,
    `</details>`
  ].join('\n');

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
    core.setFailed(`[UI Update Error]: ${e.message}`);
  }
};
