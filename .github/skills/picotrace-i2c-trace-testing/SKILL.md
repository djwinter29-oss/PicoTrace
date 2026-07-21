---
name: picotrace-i2c-trace-testing
description: 'Run PicoTrace I2C trace tests on Raspberry Pi using the board CDC CLI plus passive USB bulk decode. Use for i2cdetect-based smoke checks, validating that one Linux I2C bus scan produces 112 traced transactions, reproducing live I2C trace output, and checking monitor overruns on current or older firmware builds.'
argument-hint: '[channel] [sample-hz] [bus]'
user-invocable: true
---

# PicoTrace I2C Trace Testing

Run a repeatable PicoTrace I2C trace test from Linux using the board-local CDC CLI to configure capture and the shared bulk trace stream to decode I2C events.

## When to Use
- Validate bench I2C wiring on Raspberry Pi and PicoTrace hardware
- Reproduce a live I2C scan trace using `i2cdetect -y 1`
- Check that one I2C bus scan produces the expected `112` traced transactions
- Smoke-test I2C capture after firmware changes
- Run as required validation after firmware changes that may affect trace-path behavior
- Confirm that the monitor runs without overruns or sticky error state

## Prerequisites
- Work in the PicoTrace repository root
- Use the bench wiring from `docs/raspberry-pi-test-setup.md`
- Ensure `/dev/i2c-1` exists on the Raspberry Pi host
- Ensure the PicoTrace board enumerates over USB and exposes its CDC interface under `/dev/serial/by-id/`
- Activate the project virtual environment when running the Python helper

## Procedure
1. Check the current I2C trace baseline notes in `docs/testlog/rp2040-benchmark-baseline.md`, the recent RP2040 run history in `docs/testlog/rp2040-benchmark-testlog.md`, and the entry format in `docs/testlog/rp2040-benchmark-testlog-template.md`, then confirm the bench wiring and Raspberry Pi preconditions in `docs/raspberry-pi-test-setup.md`.
2. If needed, rebuild and flash the target firmware before testing.
3. When comparing RP2040 clock points, build one firmware directory per clock using `./tools/linux/build.sh --board pico --firmware-build-dir ... --system-clock-khz ...`, then flash the matching directory before each sweep.
4. Run the repo-local I2C trace helper to configure channel `0`, generate one `i2cdetect -y 1` scan, and collect the decoded trace summary.
5. Compare the traced transaction count against the expected `112` address-probe transactions.
6. Check the device status line for `overruns=0` and `sticky=0`.
7. Record the run in `docs/testlog/rp2040-benchmark-testlog.md` using `docs/testlog/rp2040-benchmark-testlog-template.md`, including commands, measured results, and interpretation.
8. Compare the I2C result against `docs/testlog/rp2040-benchmark-baseline.md` and report whether the trace result changed compared with the previous baseline, especially transaction count, balanced start/stop events, or monitor overrun behavior.
9. When you need a heavier I2C decode/backlog check than one `i2cdetect` scan, run the helper with `--workload combined-burst --target-address 0x50 --read-length 4 --repeat-count 64 --expect-transactions 0` and compare the repeated-start event shape (`starts = 2 * stops`) plus monitor overrun behavior.
10. When you only need the standard smoke check, prefer the baseline conservative rate for the selected firmware clock.
11. If you are checking the sampler ceiling on RP2040, use the current baseline table as the starting point: `150 MHz -> 3.75 MHz`, `200 MHz -> 5.25 MHz`, `225 MHz -> 5.75 MHz`, `250 MHz -> 6.6 MHz` clean on the standard `i2cdetect` workload.
12. If the helper fails because the live firmware or bench cannot sustain the requested rate, retry with a lower non-zero sampler rate instead of assuming only a few discrete values are supported; the current firmware accepts any non-zero `sample-hz` and clamps the PIO divider at `1.0`.

## Core Commands
- Focused firmware tests: `cmake --build build/tests --target usb_app_test && ./build/tests/usb_app_test`
- Build Pico firmware: `cmake --build build/firmware-pico --target picotrace`
- Flash Pico firmware: `./tools/linux/load.sh --board pico --firmware-build-dir build/firmware-pico --skip-build`
- Build clock-specific Pico firmware: `./tools/linux/build.sh --board pico --firmware-build-dir build/firmware-pico-225m --system-clock-khz 225000`
- Flash clock-specific Pico firmware: `./tools/linux/load.sh --board pico --firmware-build-dir build/firmware-pico-225m --skip-build`
- Build Pico 2 firmware: `cmake --build build/firmware-pico2 --target picotrace`
- Flash Pico 2 firmware: `./tools/linux/load.sh --board pico2 --firmware-build-dir build/firmware-pico2 --skip-build`
- Default I2C scan test: `./.venv/bin/python tools/linux/i2c_trace_test.py --channel 0 --bus 1 --sample-hz 4000000 --expect-transactions 112`
- Repeated-start stress test: `./.venv/bin/python tools/linux/i2c_trace_test.py --channel 0 --bus 1 --sample-hz 4000000 --workload combined-burst --target-address 0x50 --read-length 4 --repeat-count 64 --expect-transactions 0`
- Passive capture only: `./.venv/bin/python tools/linux/i2c_trace_test.py --channel 0 --sample-hz 4000000 --no-generate-traffic --expect-transactions 0`

## Interpretation
- `transactions=112` on one `i2cdetect -y 1` scan matches the expected Linux address-probe workload.
- `starts != stops` means the captured I2C event stream is incomplete or truncated.
- `overruns > 0` or `sticky > 0` means the monitor fell behind or latched an error condition during capture.
- A status line that does not enter `running` means the live firmware rejected the requested `i2cmon` configuration.
- On the current RP2040 bench, treat the baseline table in `docs/testlog/rp2040-benchmark-baseline.md` as the source of truth for clock-specific clean I2C sampler points; use those upper-edge rates only for explicit ceiling checks, not routine regression smoke tests.

## References
- Bench wiring and Raspberry Pi traffic generation: `docs/raspberry-pi-test-setup.md`
- Repo-local helper: `tools/linux/i2c_trace_test.py`
- RP2040 baseline page: `docs/testlog/rp2040-benchmark-baseline.md`
- RP2040 dated run log: `docs/testlog/rp2040-benchmark-testlog.md`
- RP2040 log template: `docs/testlog/rp2040-benchmark-testlog-template.md`