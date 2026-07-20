---
name: picotrace-i2c-trace-testing
description: 'Run PicoTrace I2C trace validation on real hardware. Use for bench bring-up, verifying `i2cdetect -y 1` traffic capture, checking the expected 112 Linux address probes, and reproducing the host-side validation flow in tools/linux/i2c_trace_test.py.'
argument-hint: '[channel] [sample-hz] [traffic-command]'
user-invocable: true
---

# PicoTrace I2C Trace Testing

Run the PicoTrace firmware and host I2C trace workflow for real hardware validation.

## When to Use
- Validate passive I2C observation on the Raspberry Pi bench
- Reproduce the `i2cdetect -y 1` capture check on Linux
- Confirm the expected `112` I2C address-probe queries from the Linux scan
- Check whether the current I2C sample-rate preset overflows under bench traffic
- Re-run I2C trace validation after firmware changes to the sampler, decoder, or buffering path

## Prerequisites
- Work in the PicoTrace repository root
- Use the bench wiring from `docs/raspberry-pi-test-setup.md`
- Ensure `/dev/i2c-1` exists on the Raspberry Pi host
- Ensure the PicoTrace board enumerates over USB
- Activate the project virtual environment when running the Python validation script

## Procedure
1. Rebuild focused firmware tests when the I2C trace path changed.
2. Rebuild and flash the target firmware before the bench run.
3. Run `tools/linux/i2c_trace_test.py` against the intended I2C monitor channel and sample-rate preset.
4. Check the decoded `START`, `DATA`, `ACK`, and `STOP` counts together with `OVERFLOW` boundaries.
5. Treat the `112` decoded address queries from `i2cdetect -y 1` as the standard Raspberry Pi address-scan reference.

## Core Commands
- Focused firmware tests: `cmake --build build/tests --target i2c_monitor_runtime_test usb_app_test && ./build/tests/i2c_monitor_runtime_test && ./build/tests/usb_app_test`
- Build Pico firmware: `cmake --build build/firmware-pico --target picotrace`
- Flash Pico firmware: `./tools/linux/load.sh --board pico --firmware-build-dir build/firmware-pico --skip-build`
- Standard Linux scan validation: `./.venv/bin/python tools/linux/i2c_trace_test.py --channel 0 --sample-hz 4000000 --traffic-command "i2cdetect -y 1" --expected-address-queries 112`

## Interpretation
- `OVERFLOW > 0` means the current I2C sampler and decode path fell behind and the address count is not trustworthy.
- `START = DATA = ACK = STOP = 112` with `OVERFLOW = 0` is the clean reference result for the Linux `i2cdetect -y 1` scan on this bench.
- A lower decoded address count with `OVERFLOW = 0` usually means the bench wiring or selected logical channel does not match the traffic source.

## References
- Bench wiring and Linux traffic generation: `docs/raspberry-pi-test-setup.md`
- Script entrypoint: `tools/linux/i2c_trace_test.py`