module.exports = async ({ github, context, core }) => {
  const { SIZE, COMMIT_MSG, TARGET_ISSUE, COMPILER_VER, ZIG_SIZE, ZIG_COMPILER_VER } = process.env;
  const fs = require('fs');
  const path = require('path');

  let VER = process.env.ATP_VERSION || 'N/A';
  let RUNTIME_VER = process.env.RUNTIME_VER || 'N/A';
  let ZIG_RUNTIME_VER = process.env.ATPD_VERSION_STRING || 'N/A';
  try {
    const versionFile = path.join(process.env.GITHUB_WORKSPACE || '.', 'include', 'version.h');
    if (fs.existsSync(versionFile)) {
      const content = fs.readFileSync(versionFile, 'utf8');
      const match = content.match(/ATP_VERSION_STRING\s+"([^"]+)"/);
      if (match) {
        VER = match[1];
        RUNTIME_VER = `atpd ${match[1]}`;
        ZIG_RUNTIME_VER = `atpd ${match[1]}`;
      }
    }
  } catch (e) {
    console.log('Could not read version.h, using env vars:', e.message);
  }

  const now = new Date();

  const localTime = new Date(now.getTime() + 8 * 3600 * 1000);
  const fmt = (v) => v.toString().padStart(2, '0');
  const ts = `${localTime.getFullYear().toString().slice(-2)}${fmt(localTime.getMonth()+1)}${fmt(localTime.getDate())} ${fmt(localTime.getHours())}:${fmt(localTime.getMinutes())}`;

  const runUrl = `https://github.com/${context.repo.owner}/${context.repo.repo}/actions/runs/${context.runId}`;
  const commitSha = context.sha.substring(0, 7);
  const commitLink = `https://github.com/${context.repo.owner}/${context.repo.repo}/commit/${context.sha}`;
  const branchName = context.ref.replace('refs/heads/', '');
  const branchLink = `https://github.com/${context.repo.owner}/${context.repo.repo}/tree/${branchName}`;

  const rawMsg = (COMMIT_MSG || 'No commit message').split('\n')[0].trim();
  const displayMsg = rawMsg.length > 42 ? `${rawMsg.substring(0, 42)}...` : rawMsg;

  const isMainBranch = (branchName === 'main' || branchName === 'dev');
  const clangLabel = isMainBranch ? `\`${COMPILER_VER || 'N/A'}\` | 📦 \`${SIZE || 'N/A'}\`` : `⏭️ skipped (feat branch)`;
  const zigLabel = `\`${ZIG_COMPILER_VER || 'N/A'}\` | 📦 \`${ZIG_SIZE || 'N/A'}\``;
  const summarySize = isMainBranch ? `${SIZE || 'N/A'} (Clang) / ${ZIG_SIZE || 'N/A'} (Zig)` : `${ZIG_SIZE || 'N/A'} (Zig)`;

  const newEntry = `
<details>
<summary>🟢 [<b><a href="${runUrl}">#${context.runNumber}</a></b>] &nbsp;&nbsp; ${ts} &nbsp;&nbsp; 📦 <b>Size:</b> ${summarySize}</summary>

### Branch: [${branchName}](${branchLink})

* 📥 **[Download Build Artifact](${runUrl})**
* 📝 **CI Version:** \`${VER}\`
* 🔧 **ATPd Version (Clang -v):** \`${RUNTIME_VER}\`
* 🔧 **ATPd Version (Zig -v):** \`${ZIG_RUNTIME_VER}\`
* ⚙️ **Clang:** ${clangLabel}
* ⚙️ **Zig CC:** ${zigLabel}
* 💬 **Message:** \`${displayMsg}\`
* 🔗 **Source:** Commit [${commitSha}](${commitLink})
* 🕒 **Build Time:** \`${ts}\`

---

</details>
`;

  try {
    const { data: issue } = await github.rest.issues.get({
      owner: context.repo.owner,
      repo: context.repo.repo,
      issue_number: parseInt(TARGET_ISSUE)
    });

    const finalBody = newEntry.trim() + "\n\n\n" + (issue.body || "");

    await github.rest.issues.update({
      owner: context.repo.owner,
      repo: context.repo.repo,
      issue_number: parseInt(TARGET_ISSUE),
      body: finalBody
    });

    console.log(`Checkpoint updated: Build #${context.runNumber} for ${branchName} | Version: ${VER}`);
  } catch (e) {
    core.setFailed(`[UI Update Error]: ${e.message}`);
  }
};