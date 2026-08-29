# ATPD Codex Autopilot — Ready-to-Use Package

This directory is meant to be merged into the root of the ATPD repository on branch `ebpf-native-api`.

## 1. Recommended location

Keep the repository inside the WSL Linux filesystem, for example:

```bash
mkdir -p ~/work
cd ~/work
# clone/switch your ATPD repository here
```

Avoid running the refactor from `/mnt/c/...` when possible.

## 2. Copy this package into the ATPD repository root

After extraction, the repository should contain:

```text
atpd/
├── CODEX_AUTOPILOT.md
├── CODEX_STEPS.md
├── .rework-state
├── .codex/
│   ├── CURRENT_ARCHITECTURE.md
│   └── steps/                  # 30 manifests
├── docs/
│   ├── refactor/               # master + all C execution plans
│   └── future/                 # Go rewrite, not part of C sequence
├── reports/
└── scripts/
    └── codex-preflight.sh
```

## 3. One-time bootstrap commit

The harness and plans are intentionally visible to Git. Commit them once before starting Step 1:

```bash
cd ~/work/atpd
chmod +x scripts/codex-preflight.sh
git status --short
git add CODEX_AUTOPILOT.md CODEX_STEPS.md .codex docs scripts/codex-preflight.sh reports/.gitkeep
git commit -m "chore(codex): add ATPD refactor autopilot harness"
```

`.rework-state` is a runtime checkpoint. You may leave it untracked. The preflight script deliberately ignores changes to `.rework-state` and `reports/` when checking whether the code working tree is clean.

If your repository policy requires all files to be tracked, you can track them; the preflight still treats their changes as runtime state. Do **not** stage them accidentally into implementation commits unless you intentionally want that history.

## 4. Verify the harness

```bash
./scripts/codex-preflight.sh
```

Expected ending:

```text
PRECHECK PASS
```

## 5. Start Codex

Give Codex this prompt:

```text
进入 WSL 中的 ATPD 仓库。

先读取 CODEX_AUTOPILOT.md，然后运行 scripts/codex-preflight.sh。

从 .rework-state 指定的 current_step 开始。
使用 CODEX_STEPS.md、.codex/CURRENT_ARCHITECTURE.md 和当前 Step manifest 作为轻量执行入口。
只读取当前 Step 对应的专项 MD；先 rg/search，再按需读取源码，不要每步重读完整 master、全部历史 reports 或全部专项 MD。

每个 Step：
审计 → 修改 → 增量编译 → 当前相关测试 → invariant 检查 → diff review → report → commit → 更新 CURRENT_ARCHITECTURE（仅持久架构事实）→ 更新 .rework-state。

PASS 自动进入下一 Step。
FAIL/hard-stop 立即停止。
同一根因最多尝试修复 3 次，不允许为了继续执行而绕过测试、隐藏失败或违反架构原则。
不需要每个 Step 询问我是否继续。
```

## 6. Important execution notes

- `ATPD_SERVICE_SUPERVISOR_OPTIMIZATION_PLAN.md` is obsolete and intentionally excluded.
- `ATPD_GO_REWRITE_PLAN.md` is in `docs/future/` and must not enter the 30-Step C sequence.
- Step 9 consumes both context plans together.
- The core-header plan is intentionally used twice: Step 3 and Step 27.
- Step 28 uses `ATPD_C_SOURCE_STABILITY_FIX_PLAN.md` as a final regression checklist, not as an early redesign plan.
- Step 30 has no additional specialized MD; it executes the final sanitizer/Android/soak release gate.
