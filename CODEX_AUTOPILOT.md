# ATPD Codex Autopilot — Token-Efficient Edition

Target branch: `ebpf-native-api`

## Required control files

```text
.rework-state
CODEX_STEPS.md
.codex/CURRENT_ARCHITECTURE.md
.codex/steps/XX-*.md
docs/refactor/<current specialized plan>.md
```

The full human master plan remains authoritative for architecture, but DO NOT reread it on every Step.
Use `CODEX_STEPS.md` + the current manifest as the machine execution path.

## Startup

1. Enter the WSL repository and read `.rework-state`.
2. Classify the checkpoint before touching source:
   - `status=ready` means `between-step`; run `scripts/codex-preflight.sh`.
   - `status=in_progress` means an explicit `resume-current-step`; first audit
     `git status`/`git diff`, then run `scripts/codex-preflight.sh --resume`.
3. Never infer resume mode from a dirty tree, and never use `--resume` to
   bypass an unknown or out-of-scope dirty file.
4. Verify the selected mode's gate passes before reading the current Step.
5. Read `CODEX_STEPS.md` only far enough to identify the current Step entry.
6. Read `.codex/CURRENT_ARCHITECTURE.md`.
7. Read the current `.codex/steps/XX-*.md`.
8. Read only the specialized MD(s) named in that manifest.
9. Run the manifest's targeted `rg` searches.
10. Open only relevant source/test hits.
11. Implement the current Step, or continue existing in-progress changes in
    resume mode without reset/checkout/clean.

## Token-efficiency rules

1. Do not reread the complete master plan every Step.
2. Do not read all specialized MDs.
3. Do not read completed reports by default.
4. Use `rg` before opening files.
5. Open narrow relevant regions instead of dumping large files where possible.
6. Expand repository scope only when symbol/ownership evidence requires it.
7. Keep successful build/test output concise.
8. Do not rerun verbose successful tests.
9. Do not restate unchanged architecture in reports.
10. Keep Step reports incremental and concise.
11. Same-root-cause repair attempts are capped at 3. If still unresolved, mark blocked and stop.
12. `repo-wide` audit permission does not mean “read every file”; search first, then inspect relevant ownership/callsites.

## Execution gate

For each Step:

```text
audit
→ modify
→ incremental build
→ relevant tests
→ regression tests
→ invariant verification
→ diff review
→ report
→ commit
→ update CURRENT_ARCHITECTURE if durable facts changed
→ update .rework-state
```

PASS may continue automatically.

FAIL/hard-stop must stop.

## Hard stops

Stop if:

- branch is not `ebpf-native-api`;
- working tree contains unrelated pre-existing modifications;
- build/test regression cannot be fixed safely within the current Step;
- 3 repair attempts for the same root cause fail;
- new UAF/double-free/zombie/FD leak/data-loss hazard cannot be resolved in-scope;
- external ABI compatibility is uncertain;
- continuing requires violating ownership/master architecture;
- deletion would break a still-valid consumer.

On failure write `reports/step-XX-failed.md`, set `.rework-state` to `status=blocked`, and stop.

## Permanent architecture rules

- One authoritative owner per resource/state.
- No new god global/singleton/common context workaround.
- Config is desired state only.
- Runtime owners expose coherent snapshots.
- Status aggregates; it does not own duplicate state.
- sing-box owns `ebpf-in`; ATPD must not restore eBPF dataplane/probe/sys_bpf ownership.
- Do not turn `atpd_context` into `atpd_global` v2.
- Do not create a replacement umbrella header.
- Do not implement future Steps early.
- `ATPD_GO_REWRITE_PLAN.md` is out of scope.
- Deprecated `ATPD_SERVICE_SUPERVISOR_OPTIMIZATION_PLAN.md` must not be used.

## Reports

Successful report:

`reports/step-XX-report.md`

Keep it to incremental facts:

- result;
- plans used;
- files changed/deleted;
- ownership/behavior changes;
- tests added;
- commands actually run;
- PASS/FAIL/N/A gates;
- deferred TODOs;
- commit hash.

Do not copy the design plan into the report.

## Resume

Use `.rework-state` as the checkpoint.
Verify `last_commit` exists and is an ancestor of HEAD before continuing.
If state and Git disagree, stop instead of guessing.

`status=ready` is a between-step checkpoint and requires the normal clean-tree
gate. Set `status=in_progress` when a Step has begun but is not yet committed;
recover it only with the explicit `scripts/codex-preflight.sh --resume` gate.
Resume accepts only reports/runtime checkpoint files plus dirty paths in the
current manifest's auditable scope. An unlisted or unexplained path is a hard
stop; resume is not a general dirty-tree override.
