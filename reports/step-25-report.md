# Step 25 Report - Status snapshot aggregation

## Result

PASS. Status now has a bounded owner-snapshot collection boundary and does not synchronously poll Native API in the render path.

## Changes

- Added `status_snapshot_t` and `status_collect_snapshot()` for one-shot ATPD/sing-box process resource collection.
- Status rendering uses the collected service PID and avoids blocking status/version/mode API calls.
- Preserved authoritative VPN/netlink/context snapshots and did not add eBPF telemetry.

## Verification

- `make -j2` PASS.
- `make test` PASS.
- `rg` confirmed no `api_get_status_sync`, `api_get_version_sync`, or `api_get_mode_sync` calls in `status.c`.
- `git diff --check` PASS.

## TODOs

- Native API cache freshness and JSON rendering remain future scope.
