# Step 26 Report - UI rendering boundary

## Result

PASS. UI output now has a centralized ANSI filtering path and UTF-8-safe truncation.

## Changes

- Made `--no-color` effective across all UI output, including hard-coded banner styles.
- Added deterministic plain-output handling for UDS status responses.
- Added defensive and codepoint-boundary-safe truncation for UTF-8 values.
- Propagated CLI no-color intent into local status rendering.

## Verification

- `make -j2` PASS (no new warnings).
- `make test` PASS.
- `atpd --no-color status` output contains no ESC bytes.
- `git diff --check` PASS.

## TODOs

- Full explicit render-context API and removal of the borrowed global output sink remain future boundary cleanup.
