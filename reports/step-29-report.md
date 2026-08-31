# Step 29 Report - Resource regression gates

## Result

PASS for implementation and repository regression gates. Resource stress execution was attempted; the current container cannot initialize ATPD netlink because it lacks `CAP_NET_ADMIN`, so the daemon-level stress run exited at startup and was cleaned up by its trap.

## Changes

- Enhanced `tests/benchmark_atpd.sh` with baseline/stress/recovery RSS, VmHWM, VmSize, PSS (graceful fallback), FD, and thread sampling in CSV form.
- Added configurable RSS/FD/thread growth thresholds and recovery warnings.
- Added `tests/stress_atpd_resources.sh` with PID-scoped cleanup, concurrent status storm, reload/restart churn, Netlink storm, and hard FD/RSS/thread growth gates.
- Removed unsafe global `pkill` cleanup from benchmark.

## Verification

- `bash -n tests/benchmark_atpd.sh tests/stress_atpd_resources.sh` PASS.
- `make test` PASS.
- `git diff --check` PASS.
- Resource stress attempted with reduced cycles; environment prerequisite failure was not hidden (`CAP_NET_ADMIN` unavailable), and temporary process/socket state was cleaned.

## TODOs

- Run full 5-10 minute stress and Android soak on a privileged runner; no production behavior was changed to accommodate the unprivileged container.
