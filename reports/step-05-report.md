# Step 05 Report - Strict config validation

- Result: PASS
- Plan used: `docs/refactor/ATPD_CONFIG_VALIDATOR_STRICTNESS_HARDENING_PLAN.md`
- Files changed: `include/config_validator.h`, `src/config_validator.c`, `src/config.c`, and `tests/test_config_validation.sh`.
- Behavior: one schema defines supported keys, types, aliases, and empty-value policy; config parsing is key-first and rejects unknown keys, invalid integers/booleans, duplicate canonical keys, malformed lines, unterminated quotes, and fixed-buffer truncation. Deprecated aliases remain accepted with warnings. Validator is const/pure and has no global strict-mode state.
- Tests added: unknown-key, invalid-type, strict-boolean, duplicate, overflow, malformed-line, and unterminated-quote cases.
- Commands run: `make -j2`; `make test`; targeted schema/parser/validator invariant `rg`; `git diff --check`.
- Gates: unknown key fails PASS; invalid numeric/bool fails PASS; no silent truncation PASS; regression PASS.
- Deferred: validation after sing-box-derived merge and active-runtime transactional reload remain Step 6; ATPD eBPF ownership removal remains Step 7.
- Commit hash: `641f1d7b92ceca4c557b5cccb1b7d0023ed6d942`.
