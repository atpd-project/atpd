# ATPD Codex Lightweight Step Index

> Machine-oriented execution index. Do not use this as the human design document.
> Read only the current Step entry plus its specialized plan and manifest.

## Reading policy

```text
Always read:
- CODEX_AUTOPILOT.md
- .rework-state
- .codex/CURRENT_ARCHITECTURE.md
- current .codex/steps/XX-*.md

Then read:
- only the current Step's specialized plan(s)
- source files discovered by the manifest's targeted searches

Do not reread:
- the full 30-Step master plan on every Step
- completed reports unless explicitly needed
- unrelated specialized MDs
```

Audit modes:
- `local`: open primary files first; targeted search around their symbols.
- `repo-symbol`: run targeted `rg` across src/include/tests; open only hits.
- `repo-wide`: broader repo audit is permitted because deletion/ownership spans many modules.

## Step 01 — Result model
- Manifest: `.codex/steps/01-result-model.md`
- Plan: `ATPD_RESULT_ERROR_MODEL_REFACTOR_PLAN.md`
- Audit: `repo-symbol`
- Commit: `refactor(result): clarify internal result model`
- Core gates: No ATP_ERR_EBPF; Internal negative result is not leaked as process exit/wire code; Result status and diagnostic history are distinct

## Step 02 — Version single source
- Manifest: `.codex/steps/02-version-single-source.md`
- Plan: `ATPD_VERSION_SINGLE_SOURCE_RELEASE_PLAN.md`
- Audit: `local`
- Commit: `refactor(version): establish single version source`
- Core gates: One canonical product version source; No __TIME__ build identity; CLI/status consume version API

## Step 03 — Core header cleanup pass 1
- Manifest: `.codex/steps/03-core-header-cleanup-pass-1.md`
- Plan: `ATPD_CORE_HEADER_OWNERSHIP_CLEANUP_PLAN.md`
- Audit: `repo-symbol`
- Commit: `refactor(headers): reduce core umbrella ownership`
- Core gates: Fortify controlled by build flags; Compatibility macros removed; Do not require deleting atp.h yet

## Step 04 — Config model immutability
- Manifest: `.codex/steps/04-config-model-immutability.md`
- Plan: `ATPD_CONFIG_MODEL_IMMUTABILITY_REFACTOR_PLAN.md`
- Audit: `repo-symbol`
- Commit: `refactor(config): make configuration value immutable`
- Core gates: Config contains desired state only; No mutex inside config value; No runtime readiness/VPN observation in config

## Step 05 — Strict config validation
- Manifest: `.codex/steps/05-strict-config-validation.md`
- Plan: `ATPD_CONFIG_VALIDATOR_STRICTNESS_HARDENING_PLAN.md`
- Audit: `local`
- Commit: `fix(config): enforce strict typed validation`
- Core gates: Unknown key fails; Invalid numeric/bool fails; No silent truncation

## Step 06 — Transactional reload
- Manifest: `.codex/steps/06-transactional-reload.md`
- Plan: `ATPD_CONFIG_TRANSACTIONAL_RELOAD_PLAN.md`
- Audit: `repo-symbol`
- Commit: `refactor(config): implement transactional reload`
- Core gates: Failed reload preserves active runtime; Final merged candidate is validated; Generation advances only on successful commit

## Step 07 — Remove ATPD-owned eBPF
- Manifest: `.codex/steps/07-remove-atpd-owned-ebpf.md`
- Plan: `ATPD_EBPF_MODULE_REMOVAL_PLAN.md`
- Audit: `repo-wide`
- Commit: `refactor(ebpf): remove ATPD-owned dataplane management`
- Core gates: No ATPD sys_bpf/probe ownership; sing-box remains sole ebpf-in dataplane owner; No fake ATPD eBPF telemetry

## Step 08 — Eliminate global runtime container
- Manifest: `.codex/steps/08-eliminate-global-runtime-container.md`
- Plan: `ATPD_GLOBAL_STATE_ELIMINATION_PLAN.md`
- Audit: `repo-wide`
- Commit: `refactor(core): eliminate global runtime container`
- Core gates: No g_atpd; No atpd_global container; Do not move everything into context

## Step 09 — Shrink context ownership
- Manifest: `.codex/steps/09-shrink-context-ownership.md`
- Plan: `ATPD_CONTEXT_STATE_OWNERSHIP_REFACTOR_PLAN.md + ATPD_CONTEXT_PUBLIC_BOUNDARY_REFACTOR_PLAN.md`
- Audit: `repo-wide`
- Commit: `refactor(context): shrink runtime context ownership`
- Core gates: No public mutable g_atpd_ctx; Context owns no sessions/eBPF/XFRM fd; Reload does not reset daemon uptime

## Step 10 — Deterministic init/shutdown rollback
- Manifest: `.codex/steps/10-deterministic-init-shutdown-rollback.md`
- Plan: `ATPD_INIT_SHUTDOWN_ROLLBACK_HARDENING_PLAN.md`
- Audit: `repo-symbol`
- Commit: `refactor(lifecycle): make startup rollback deterministic`
- Core gates: Startup failure returns nonzero; Completed phases rollback in reverse order; No async-stop/free UAF

## Step 11 — Slim main lifecycle orchestration
- Manifest: `.codex/steps/11-slim-main-lifecycle-orchestration.md`
- Plan: `ATPD_MAIN_LIFECYCLE_ORCHESTRATION_REFACTOR_PLAN.md`
- Audit: `local`
- Commit: `refactor(main): reduce daemon lifecycle orchestration`
- Core gates: Main does not manipulate service internals; Daemon parent cannot report false startup success; STOPPED published after teardown

## Step 12 — Reactor lifecycle hardening
- Manifest: `.codex/steps/12-reactor-lifecycle-hardening.md`
- Plan: `ATPD_REACTOR_STABILITY_HARDENING_PLAN.md`
- Audit: `local`
- Commit: `fix(reactor): harden event lifecycle and ownership`
- Core gates: FD/timer/callback ownership is explicit; Create/add failures propagate; Destroy leaves no dangling registrations

## Step 13 — Service supervisor ownership
- Manifest: `.codex/steps/13-service-supervisor-ownership.md`
- Plan: `ATPD_SERVICE_C_REFACTOR_PLAN.md`
- Audit: `repo-symbol`
- Commit: `refactor(service): establish child supervisor ownership`
- Core gates: Service owns child PID/reap/restart; No name-based child ownership; One stop/restart implementation

## Step 14 — Native API transport reliability
- Manifest: `.codex/steps/14-native-api-transport-reliability.md`
- Plan: `ATPD_SINGBOX_NATIVE_API_RELIABILITY_PLAN.md`
- Audit: `local`
- Commit: `refactor(singbox-api): harden native API transport`
- Core gates: Transport lifecycle has one owner; Consumers read cached snapshot; Unavailable API does not block daemon

## Step 15 — Thin API control facade
- Manifest: `.codex/steps/15-thin-api-control-facade.md`
- Plan: `ATPD_API_CONTROL_BOUNDARY_REFACTOR_PLAN.md`
- Audit: `repo-symbol`
- Commit: `refactor(api): make control API a thin facade`
- Core gates: API does not own transport; No synchronous sleep retry loop; Desired state is separate from observation/reconcile

## Step 16 — Netlink/XFRM ownership
- Manifest: `.codex/steps/16-netlink-xfrm-ownership.md`
- Plan: `ATPD_NETLINK_XFRM_STABILITY_HARDENING_PLAN.md`
- Audit: `local`
- Commit: `refactor(netlink): own XFRM observation lifecycle`
- Core gates: Netlink owns its FD and registration; Context owns no XFRM fd/current VPN iface; Snapshot fields are coherent

## Step 17 — Session lifecycle ownership
- Manifest: `.codex/steps/17-session-lifecycle-ownership.md`
- Plan: `ATPD_SESSION_LIFECYCLE_OWNERSHIP_HARDENING_PLAN.md`
- Audit: `repo-symbol`
- Commit: `refactor(session): centralize session lifecycle ownership`
- Core gates: Session manager owns registry; VPN teardown uses one close-all path; No double destroy; >256 sessions supported

## Step 18 — Splice datapath consolidation
- Manifest: `.codex/steps/18-splice-datapath-consolidation.md`
- Plan: `ATPD_SPLICE_DATAPATH_CONSOLIDATION_PLAN.md`
- Audit: `repo-symbol`
- Commit: `refactor(splice): consolidate stream datapath`
- Core gates: No duplicate production datapath; Fairness budget cannot permanently stall edge-triggered IO; Partial transfer preserves byte integrity

## Step 19 — Async validator lifecycle
- Manifest: `.codex/steps/19-async-validator-lifecycle.md`
- Plan: `ATPD_ASYNC_VALIDATE_LIFECYCLE_HARDENING_PLAN.md`
- Audit: `local`
- Commit: `fix(async-validate): unify child completion lifecycle`
- Core gates: Child reaped exactly once; EOF/timeout race cannot hang; Shutdown can cancel without zombies

## Step 20 — UDS lifecycle reliability
- Manifest: `.codex/steps/20-uds-lifecycle-reliability.md`
- Plan: `ATPD_UDS_RELIABILITY_HARDENING_PLAN.md`
- Audit: `local`
- Commit: `fix(uds): harden local control socket lifecycle`
- Core gates: No accepted-FD leaks; Idle/slow clients are bounded; Partial responses are handled correctly

## Step 21 — Diagnostic event history
- Manifest: `.codex/steps/21-diagnostic-event-history.md`
- Plan: `ATPD_ERROR_DIAGNOSTICS_HARDENING_PLAN.md`
- Audit: `repo-symbol`
- Commit: `refactor(error): centralize diagnostic event history`
- Core gates: One diagnostic history owner; Copy-out getters; No logging while holding diagnostic lock

## Step 22 — Logger reliability
- Manifest: `.codex/steps/22-logger-reliability.md`
- Plan: `ATPD_LOGGER_RELIABILITY_HARDENING_PLAN.md`
- Audit: `local`
- Commit: `fix(logger): harden logging state and file safety`
- Core gates: No level OOB; Minimum level access is race-safe; Logger has no recursive atpd_error dependency

## Step 23 — Utils/platform safety
- Manifest: `.codex/steps/23-utils-platform-safety.md`
- Plan: `ATPD_UTILS_PLATFORM_SAFETY_REFACTOR_PLAN.md`
- Audit: `repo-wide`
- Commit: `MULTI-COMMIT: follow master plan substeps`
- Core gates: Command timeout covers full child lifecycle; Process identity uses PID+starttime semantics; Platform/path helpers are safe; utils becomes generic

## Step 24 — Strict CLI parsing
- Manifest: `.codex/steps/24-strict-cli-parsing.md`
- Plan: `ATPD_CLI_STRICT_PARSING_REFACTOR_PLAN.md`
- Audit: `local`
- Commit: `refactor(cli): enforce strict command parsing`
- Core gates: Invalid/trailing arguments fail; CLI options are not config state; Exit/version mapping is stable

## Step 25 — Status snapshot aggregation
- Manifest: `.codex/steps/25-status-snapshot-aggregation.md`
- Plan: `ATPD_STATUS_OBSERVABILITY_REFACTOR.md`
- Audit: `repo-symbol`
- Commit: `refactor(status): aggregate owner snapshots`
- Core gates: Status aggregates authoritative snapshots only; No fake eBPF metrics; No duplicate readiness/stats ownership

## Step 26 — UI rendering boundary
- Manifest: `.codex/steps/26-ui-rendering-boundary.md`
- Plan: `ATPD_UI_RENDERING_BOUNDARY_HARDENING_PLAN.md`
- Audit: `local`
- Commit: `refactor(ui): isolate presentation rendering`
- Core gates: No global output sink/config read; Plain/UDS output has no ANSI; UTF-8 truncation is safe

## Step 27 — Core header final cleanup
- Manifest: `.codex/steps/27-core-header-final-cleanup.md`
- Plan: `ATPD_CORE_HEADER_OWNERSHIP_CLEANUP_PLAN.md`
- Audit: `repo-wide`
- Commit: `refactor(headers): remove legacy umbrella header`
- Core gates: Public headers self-contained; No legacy atp.h dependency if removable; No replacement umbrella header

## Step 28 — Whole-repo stability checklist
- Manifest: `.codex/steps/28-whole-repo-stability-checklist.md`
- Plan: `ATPD_C_SOURCE_STABILITY_FIX_PLAN.md`
- Audit: `repo-wide`
- Commit: `fix(stability): close remaining lifecycle regressions`
- Core gates: All remaining P0/P1 checklist items resolved/tested/documented; Do not redesign already-stable architecture

## Step 29 — Resource regression gates
- Manifest: `.codex/steps/29-resource-regression-gates.md`
- Plan: `ATPD_RESOURCE_TESTING_IMPLEMENTATION.md`
- Audit: `repo-wide`
- Commit: `test(stability): add resource regression gates`
- Core gates: Baseline/stress/recovery metrics recorded; Leak slope and FD/thread growth gates exist; Crash/reload/session churn covered

## Step 30 — RC/stable validation
- Manifest: `.codex/steps/30-rc-stable-validation.md`
- Plan: `NONE — use master invariants`
- Audit: `repo-wide`
- Commit: `test(release): complete RC stability matrix`
- Core gates: Sanitizer matrix completed where supported; Android recovery/transition scenarios documented; Stable requires soak/release gate, not build-only
