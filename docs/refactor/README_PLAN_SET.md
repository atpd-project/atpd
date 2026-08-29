# ATPD C Refactor Plan Set

Execution source of truth:

- Human master: `ATPD_C_REFACTOR_MASTER_EXECUTION_PLAN.md`
- Machine index: repository root `CODEX_STEPS.md`
- Per-Step machine manifests: repository root `.codex/steps/`

## Plan-set audit

- 30 execution Steps are covered.
- Step 9 intentionally consumes two context plans together.
- Step 3 and Step 27 intentionally reuse the core-header plan.
- Step 30 intentionally has no new specialized plan.
- `ATPD_C_SOURCE_STABILITY_FIX_PLAN.md` is present for Step 28.
- The obsolete `ATPD_SERVICE_SUPERVISOR_OPTIMIZATION_PLAN.md` is excluded.
- The Go rewrite plan is stored under `docs/future/` and is outside the C execution sequence.
