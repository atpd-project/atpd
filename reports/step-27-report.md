# Step 27 Report - Core header final cleanup

## Result

PASS. The second header pass removed unnecessary legacy umbrella dependencies from CLI and UI implementation units.

## Changes

- `src/cli.c` now uses the canonical version API without importing `atp.h`.
- `src/ui.c` no longer imports the core umbrella header; its dependencies are local/standard only.
- No replacement `common.h`, `base.h`, or `all.h` was introduced.

## Verification

- `make -j2` PASS.
- Targeted include audit found no umbrella replacement and no obsolete eBPF core symbols.
- `git diff --check` PASS.

## TODOs

- Remaining owner-specific `atp.h` consumers require broader API ownership changes outside this second-pass scope.
