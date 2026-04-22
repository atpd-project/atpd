module.exports = async ({ github, context, core }) => {
  const { SIZE, VER, COMMIT_MSG, TARGET_ISSUE } = process.env;
  const now = new Date();
  
  // 强制北京时间 (UTC+8)
  const localTime = new Date(now.getTime() + 8 * 3600 * 1000);
  const fmt = (v) => v.toString().padStart(2, '0');
  
  const ts = `${localTime.getFullYear().toString().slice(-2)}${fmt(localTime.getMonth()+1)}${fmt(localTime.getDate())} ${fmt(localTime.getHours())}:${fmt(localTime.getMinutes())}`;
  const runUrl = `https://github.com/${context.repo.owner}/${context.repo.repo}/actions/runs/${context.runId}`;
  
  const commitSha = context.sha.substring(0, 7);
  const commitLink = `https://github.com/${context.repo.owner}/${context.repo.repo}/commit/${context.sha}`;

  // ⚠️ 核心修复：
  // 1. summary 后面必须跟两个换行符
  // 2. 所有的 Markdown 列表符号 (*) 前面不留任何空格
  // 3. 标签闭合前也保留空行
  const newEntry = [
    `<details>`,
    `<summary>🟢 <b>[#${context.runNumber}](${runUrl})</b> &nbsp;&nbsp; ${ts} &nbsp;&nbsp; 📦 <b>Size:</b> ${SIZE}</summary>`,
    '',
    `### Build Delivery (Decoupled)`,
    '',
    `* 📥 **[Download Build Artifact](${runUrl})**`,
    `* 📝 **ATPd Version:** \`${VER}\``,
    `* 🔗 **Source:** Commit [${commitSha}](${commitLink})`,
    `* 🕒 **Build Time:** \`${ts}\``,
    '',
    `---`,
    '',
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
