# ATPD Codex Handoff

Repository: `/home/ezhang/atpd`
Branch: `ebpf-native-api`
HEAD: `e7311b4`

Last completed Step: 20
Next Step: 21

Current `.rework-state`:

```text
current_step=21
last_completed_step=20
last_commit=e7311b496b1aa3817a4d7c86c090b056fed00608
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

- Step 20: `e7311b4 fix(uds): harden local control socket lifecycle`
- Step 17: `c95a2f2 refactor(session): centralize session lifecycle ownership`
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

Unattended unsigned commits: `bf9c6ef`, `c21f3be`, `167b256`, `4114564`, `e36b8b9`, `2ba27c9`, `c95a2f2`, `34ca744`, `7788c02`.
