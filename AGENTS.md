# ATPD Repository Instructions

## Ponytail scope discipline

- Make the smallest correct change.
- Prefer deletion over addition.
- Prefer existing project helpers over new abstractions.
- Prefer standard library and platform capabilities over new dependencies.
- Do not introduce a dependency unless it is strictly necessary for the requested objective.
- Do not generalize for hypothetical future requirements.
- Do not refactor unrelated code or perform opportunistic cleanup.
- Do not add wrappers, managers, frameworks, or compatibility layers unless the task requires them.
- Treat one task as one objective. Do not expand its scope.
- Inspect only files directly relevant to the objective, then widen inspection only when concrete evidence requires it.
- Run only the minimal relevant tests and required acceptance checks.
- Stop once the acceptance criteria pass.
- Prefer high-confidence findings over quantity. If evidence is incomplete, mark it as needing verification instead of asserting a bug.

## ATPD architecture and delivery boundaries

- ATPD remains the system-routing control daemon.
- sing-box `ebpf-in` owns the eBPF dataplane.
- Do not duplicate eBPF program or map lifecycle in ATPD.
- Preserve authoritative runtime-state ownership boundaries: mutable state stays with its designated owner, and consumers use read-only snapshots or accessors.
- Config represents desired state only; do not use it as a runtime-state container.

## Build and CI discipline

- Treat validated build recipes as authoritative.
- Do not alter verified build commands unless explicitly requested.
- Do not replace a known-good release build path with a default `make` path unless explicitly requested.
- Never rename product binaries or artifacts to include implementation details such as `-zig`.
- Do not perform toolchain audits unless directly required by the task.
- When a CI failure occurs, diagnose the failing gate first. Do not broaden the task into unrelated architecture, build-system, or toolchain changes.
- Do not weaken CI gates with `|| true`, `continue-on-error`, or equivalent behavior unless explicitly requested.

## Repository-history discipline

- Do not treat historical refactor documents, reports, old harness files, or past implementation plans as current requirements unless explicitly referenced.
- Current source code and current project instructions take precedence over historical refactor artifacts.

## Git discipline

- Git-only tasks must remain Git-only.
- Do not use GPG signing unless the user explicitly requests it.
- Do not reset, rebase, force-push, or clean unrelated files unless explicitly authorized.
- Do not stage unrelated or untracked files.
