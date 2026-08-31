# Step 09 Report - Shrink context ownership

Result: PASS

Plans used:

- `.codex/steps/09-shrink-context-ownership.md`
- `docs/refactor/ATPD_CONTEXT_STATE_OWNERSHIP_REFACTOR_PLAN.md`
- `docs/refactor/ATPD_CONTEXT_PUBLIC_BOUNDARY_REFACTOR_PLAN.md`

Changes:

- Made `atpd_context_t` opaque and removed the public mutable `g_atpd_ctx` object.
- Reduced context ownership to lifecycle state and mutex-protected VPN observation snapshots.
- Made daemon uptime monotonic and init-only; reload and runtime transitions do not reset it.
- Validated runtime transitions and restricted reload eligibility to `RUNNING`.
- Moved session registry, active count, and emergency drain ownership into `session.c` with registry references.
- Removed context-owned XFRM FD, generic readiness/statistics, duplicate error storage, and session list declarations.
- Status and UDS now read owner APIs for VPN, reactor, session, and error data.

Verification:

- Baseline `./scripts/codex-preflight.sh`: PASS before Step 8 worktree changes.
- Incremental `make`: PASS
- `make test`: PASS, including the new context ownership test.
- CLI smoke checks for `help` and `status`: PASS
- Ownership residual search: PASS
- `git diff --check`: PASS

Gates:

- No public mutable `g_atpd_ctx`: PASS
- Context owns no sessions, eBPF state, or XFRM FD: PASS
- Reload does not reset daemon uptime: PASS

Deferred TODOs:

- Step 10 retains responsibility for deterministic init/shutdown/rollback ordering.
- Step 11 retains responsibility for slimming main lifecycle orchestration and publishing STOPPED after teardown.

Commit hash: recorded after implementation commit.
