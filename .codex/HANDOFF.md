# ATPD Codex Handoff

Repository: `/home/ezhang/atpd`
Branch: `ebpf-native-api`
HEAD: `1a512f77e7131e8f9aa5744e796ac955790047d5`

Last completed Step: 11
Next Step: 12

Current `.rework-state`:

```text
current_step=12
last_completed_step=11
last_commit=041fbea
status=ready
blocked_reason=
```

Working tree: product source is clean. The only remaining changes are the preserved, untracked historical reports:

```text
?? reports/step-01-report.md
?? reports/step-02-report.md
?? reports/step-03-report.md
```

Key commits:

- Step 11: `041fbea3 refactor(main): reduce daemon lifecycle orchestration`
- Resume-gate harness: `1a512f77 chore(codex): add explicit current-step resume gate`

Authoritative navigation:

- Architecture checkpoint: `.codex/CURRENT_ARCHITECTURE.md`
- Automation rules: `CODEX_AUTOPILOT.md` and `CODEX_STEPS.md`
- Step manifests: `.codex/steps/`

Preflight:

- Between-step ready: `./scripts/codex-preflight.sh`
- Explicit current-step in-progress resume: `./scripts/codex-preflight.sh --resume`

Use `--resume` only for a clearly identified current-step in-progress tree. It must not be used to bypass unknown or out-of-scope dirty files; resume audit must reject those files.

Old Codex conversation history is not required for recovery. Git, `.rework-state`, this file, `.codex/CURRENT_ARCHITECTURE.md`, and the current Step manifest are the authoritative state sources.

Known unresolved issues: none.

Preserved historical reports remain untracked and must not be modified or removed as part of startup or handoff. Treat them as historical artifacts, not as current implementation state.
