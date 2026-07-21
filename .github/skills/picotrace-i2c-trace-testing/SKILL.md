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
1. Check the current I2C trace baseline notes in `docs/rp2040-benchmark.md`, then confirm the bench wiring and Raspberry Pi preconditions in `docs/raspberry-pi-test-setup.md`.
2. If needed, rebuild and flash the target firmware before testing.
3. Run the repo-local I2C trace helper to configure channel `0`, generate one `i2cdetect -y 1` scan, and collect the decoded trace summary.
4. Compare the traced transaction count against the expected `112` address-probe transactions.
5. Check the device status line for `overruns=0` and `sticky=0`.
6. Use `docs/rp2040-benchmark.md` as the comparison report page and report whether the I2C trace result changed compared with the previous baseline, especially transaction count, balanced start/stop events, or monitor overrun behavior.
7. If the helper fails because the live firmware accepts different CDC `i2cmon` sample-rate values, retry with a supported sampler rate such as `4000000`, `8000000`, or `12000000`.

## Core Commands
- Focused firmware tests: `cmake --build build/tests --target usb_app_test && ./build/tests/usb_app_test`
- Build Pico firmware: `cmake --build build/firmware-pico --target picotrace`
- Flash Pico firmware: `./tools/linux/load.sh --board pico --firmware-build-dir build/firmware-pico --skip-build`
- Build Pico 2 firmware: `cmake --build build/firmware-pico2 --target picotrace`
- Flash Pico 2 firmware: `./tools/linux/load.sh --board pico2 --firmware-build-dir build/firmware-pico2 --skip-build`
- Default I2C scan test: `./.venv/bin/python tools/linux/i2c_trace_test.py --channel 0 --bus 1 --sample-hz 4000000 --expect-transactions 112`
- Passive capture only: `./.venv/bin/python tools/linux/i2c_trace_test.py --channel 0 --sample-hz 4000000 --no-generate-traffic --expect-transactions 0`

## Interpretation
- `transactions=112` on one `i2cdetect -y 1` scan matches the expected Linux address-probe workload.
- `starts != stops` means the captured I2C event stream is incomplete or truncated.
- `overruns > 0` or `sticky > 0` means the monitor fell behind or latched an error condition during capture.
- A status line that does not enter `running` means the live firmware rejected the requested `i2cmon` configuration.

## References
- Bench wiring and Raspberry Pi traffic generation: `docs/raspberry-pi-test-setup.md`
- Repo-local helper: `tools/linux/i2c_trace_test.py`