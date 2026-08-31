# Step 24 Report - Strict CLI parsing

## Result

PASS. CLI parsing now fails closed and exposes typed run-mode/verbosity intent.

## Changes

- Replaced mutually exclusive daemon/foreground and verbose/quiet booleans with enums.
- Rejected overlong `--config`/`--pid` paths instead of truncating.
- Added strict missing/unknown option handling and rejected unknown/trailing positional arguments.
- Rejected conflicting options and options that do not apply to the selected command.
- Removed unused `--force` and `--test` options after callsite audit.
- Removed the CLI header's dependency on `logger.h`; logger mapping remains in initialization.
- Made help/version return before configuration loading.

## Verification

- `./scripts/codex-preflight.sh` PASS before implementation.
- `make -j2` PASS.
- `make test` PASS.
- Manual CLI checks: help/version success without initialization; trailing args, unknown/missing options, mode/verbosity conflicts, invalid command options, and `--version start` all fail.
- `rg` invariant audit found no old option fields or obsolete eBPF CLI strings.
- `git diff --check` PASS.

## TODOs

- No future-Step work performed.
