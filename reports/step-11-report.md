# Step 11 Report - Slim main lifecycle orchestration

## Result

PASS. Daemon-mode startup now reports success to its parent only after the
required control-plane setup and service start checks complete. Shutdown state
publication remains after teardown.

## Changed behavior and ownership

- Added a CLOEXEC startup handshake across the existing double-fork path;
  READY is reported only after UDS setup, service start, and initial VPN
  reconcile, while startup failure reports nonzero.
- Kept service child and timer ownership behind service APIs; `main.c` does
  not access service internals or perform service child kill/reap operations.
- Made reload enter `RELOADING` when the control loop begins the operation,
  not when SIGHUP is received; shutdown suppresses pending reload/status work.
- Made restart stop-failure aware so it does not start a second instance when
  the existing daemon was not stopped successfully.

## Verification

- Incremental `make -j2`: PASS
- `make test`: PASS
- Daemon startup failure propagation smoke: PASS (nonzero parent exit)
- `build/bin/atpd help`, `version`, and `status`: PASS
- Service-internal access and lifecycle ordering searches: PASS
- `git diff --check`: PASS

## Gates

- Main does not manipulate service internals: PASS
- Daemon parent cannot report false startup success: PASS by startup handshake
- `STOPPED` published after teardown: PASS; publication follows UDS/session,
  service, API, netlink, and reactor cleanup

## TODOs

- Deeper reactor, service, control API, netlink, session, UDS, and PID identity
  hardening remains assigned to later manifest steps; no Step 12 work was
  started.

## Commit

`041fbea refactor(main): reduce daemon lifecycle orchestration`
