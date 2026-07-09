# Publication prep checklist

Working tracker for the pre-publication review pass (delete before final public push,
or keep as a record — your call). Generated 2026-06-25.

## Blockers (must fix before posting)
- [x] Add MIT LICENSE file at repo root (copyright holder = "Lukas Severinghaus" — change to Freefly Systems if that's the correct owner)
- [x] Credentials → env vars: `set-hmi-url.sh`, `test_live.py`, `test_multirack.py`
- [x] Internal network names → placeholders: `app_state.cpp`, `config_store.cpp`, `dev-restart.sh` (also location_id 42→0)
- [x] Fix stale broken `set-hmi-url.sh` `/provision` call (now takes optional rack-pk arg)

## Correctness bugs
- [x] `core-fw/src/main.cpp` — require successful `FW_VERIFY` before `FW_COMMIT` (new `s_fw_verified` gate)
- [x] `esp32-hmi/src/net/wifi_mgr.cpp` — WiFi event callback now marshals online state via `dispatch_on_lvgl`
- [x] `inventree-plugin/smart_reel/api.py` — `token.set_metadata(...)` moved inside `transaction.atomic()`
- [x] `inventree-plugin/smart_reel/services.py` — `enqueue_locate` now locks the rack row (`select_for_update`)

## Docs cleanup
- [x] Removed `docs/review-feedback.md`, `docs/review-feedback-solutions.md`, `docs/test-plan.md`
- [x] Fixed the `test-plan.md` reference in `docs/README.md`

## Unit tests
- [x] RS485 frame/protocol codec host tests (`tests/`, `make`) — 67 checks pass
- [x] Plugin `services.py` pytest suite (`inventree-plugin/test_services.py`) — 10 tests pass

## Done earlier
- [x] README AI disclaimer + security note added; Tests section added

## NOT done (out of scope for this pass — flagged for your call)
- [ ] TLS `setInsecure()` in `inv_api.cpp:92` — still disabled; now disclaimed in README + known-bugs. Implement cert pinning before untrusted-network deployment.
- [ ] Rename misleading `mock_*` state mutators in `app_state.cpp` (production code, not test scaffolding).
- [ ] 30+ `// review item N/R/B` code comments reference the now-removed `docs/review-feedback.md`; harmless but orphaned.
- [ ] `inv_api.h:5` header comment says "HTTP or HTTPS"; runtime is HTTPS-only.
- [ ] `core-fw` symlinked shared headers won't materialize on a default Windows `git clone` — worth a build note.
- [ ] Firmware was NOT recompiled here (no PlatformIO toolchain/hardware in this environment); edits are minimal/surgical — build before flashing.
- [ ] Confirm copyright holder in LICENSE / `pyproject.toml` author email before publishing.
