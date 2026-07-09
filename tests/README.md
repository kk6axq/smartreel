# Tests

Lightweight, host-runnable tests for the parts of SmartReel that can be checked
without hardware or a live InvenTree. They are deliberately dependency-free so
they run anywhere (and in CI) with just a C++ compiler and Python.

This is **not** full coverage — the firmware UI/RS485 transport and the plugin's
model-touching code are validated on real hardware and against a running
InvenTree (`inventree-plugin/test_live.py`, `test_multirack.py`). These suites
cover the shared, pure logic that is easy to break and expensive to debug on the
bench.

## RS485 frame codec (C++)

The frame encoder/decoder + CRC in `esp32-hmi/src/rs485/` is the wire format
spoken by *both* firmwares, so a regression breaks the whole bus. It is plain,
platform-independent C++ and compiles on the host:

```bash
cd tests
make            # builds and runs; exit code 0 on success
```

Requires any C++17 compiler (`g++`/`clang++`). No PlatformIO toolchain needed.

## Plugin services (Python)

Pure logic in `inventree-plugin/smart_reel/services.py` — op_id idempotency,
wire-id mapping, pick-job status — tested with stubbed Django symbols so no
InvenTree install is required:

```bash
cd inventree-plugin
python3 test_services.py        # self-running, no deps
# or, if you have pytest:
pytest test_services.py
```
