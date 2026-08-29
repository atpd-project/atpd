# Step 04 Report - Config model immutability

- Result: PASS
- Plan used: `docs/refactor/ATPD_CONFIG_MODEL_IMMUTABILITY_REFACTOR_PLAN.md`
- Files changed: `include/atp_config.h`, `include/config.h`, `include/ebpf.h`, `include/atp.h`, `src/config.c`, `src/config_validator.c`, `src/atpd_init.c`, `src/main.c`, `src/ebpf.c`, `Makefile`, and `tests/test_config_value.c`.
- Behavior: `atp_config_t` is a plain desired-state value without CLI state, runtime readiness/VPN observation, eBPF enablement, or mutex ownership; service restart delay is owned by `service_config_t`; config loading and reload use value assignment.
- Tests added: plain configuration value copy smoke test.
- Commands run: `make -j2`; `make test`; targeted config invariant `rg`; `git diff --check`.
- Gates: config contains desired state only PASS; no mutex inside config value PASS; no runtime readiness/VPN observation in config PASS; regression PASS.
- Deferred: strict typed validation remains Step 5; transactional reload semantics remain Step 6; ATPD eBPF module ownership removal remains Step 7.
- Commit hash: `ea592059f7b2251be996482a33ea6d01c987639f`.
