# Step 10 Report - Deterministic init/shutdown rollback

## Result

PASS. Startup failure paths now return nonzero, completed initialization phases
are rolled back in reverse order, and service teardown no longer frees a
context after starting an asynchronous stop.

## Changed behavior and ownership

- Added reactor as a first-class initialization phase.
- Recorded completed and degraded phases; rollback invokes only cleanup for
  successfully completed phases in reverse order.
- Added synchronous service stop/destroy primitives that cancel owned timers,
  stop and reap the owned child, reset service state, and remove its PID file.
- Centralized service shutdown in `service.c`; `main.c` no longer manipulates
  service timers, child PID, or wait/reap state.
- Ordered normal teardown as UDS, sessions, service, API, netlink, reactor,
  then publishes `STOPPED`.
- Removed the no-op atexit cleanup shim and its Android build entry.
- Ensured daemon context initialization occurs once in the start path and
  configuration is loaded once by command dispatch.

## Verification

- `make -B -j2`: PASS
- `make test`: PASS
- `build/bin/atpd help` and `build/bin/atpd status`: PASS
- Targeted ownership/init-count/residual cleanup searches: PASS
- `git diff --check`: PASS

## Gates

- Startup failure returns nonzero: PASS
- Completed phases roll back in reverse order: PASS by completed-phase mask
  and registered cleanup callbacks
- No async-stop/free UAF: PASS; rollback and normal destruction use
  `service_stop_sync()` before `free()`

## TODOs

- Future lifecycle steps retain ownership of deeper reactor, service, API,
  netlink, session, and UDS hardening; no future-step implementation was added
  here.

## Commit

`114c7c5 refactor(lifecycle): make startup rollback deterministic`
