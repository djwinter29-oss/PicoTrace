---
name: picotrace-benchmarking
description: 'Run PicoTrace benchmark tests for RP2040 and RP2350 firmware. Use for SPI throughput benchmarking, benchmark bring-up, flashing firmware before tests, comparing 200 MHz and 225 MHz Pico baselines, checking MOSI or MOSI+MISO pass rates, and collecting benchmark evidence from tools/linux/spi_trace_benchmark.py.'
argument-hint: '[board] [capture] [speeds]'
user-invocable: true
---

# PicoTrace Benchmarking

Run the PicoTrace firmware and host benchmark workflow for measured SPI throughput on real hardware.

## When to Use
- Run SPI benchmark tests on Raspberry Pi Pico or Pico 2
- Rebuild and flash firmware before a benchmark run
- Compare MOSI versus MOSI+MISO throughput
- Validate benchmark regressions after SPI sampler or packetization changes
- Run as required validation after firmware changes that may affect trace-path performance
- Reproduce the documented RP2040 benchmark sweeps
- Record fresh measured results in the RP2040 benchmark test log
- Update the stable RP2040 baseline page only when the baseline itself should change

## Prerequisites
- Work in the PicoTrace repository root
- Use the bench wiring from `docs/raspberry-pi-test-setup.md`
- Ensure `/dev/spidev0.0` exists on the Raspberry Pi host
- Ensure the PicoTrace board enumerates over USB
- Activate the project virtual environment when running the Python benchmark script

## Procedure
1. Check the current RP2040 baseline notes in `docs/testlog/rp2040-benchmark-baseline.md`, the live run log in `docs/testlog/rp2040-benchmark-testlog.md`, and the entry format in `docs/testlog/rp2040-benchmark-testlog-template.md` before changing the measured baseline.
2. Rebuild focused firmware tests when the SPI trace path changed.
3. Unless the user explicitly asks for another clock or a cross-clock comparison, use the `250 MHz` RP2040 build directory `build/firmware-pico-250m` for the benchmark run.
4. When comparing RP2040 clock points, build one firmware directory per clock using `./tools/linux/build.sh --board pico --firmware-build-dir ... --system-clock-khz ...`.
5. Rebuild and flash the target firmware before benchmarking.
6. Run the SPI benchmark script with the correct board, capture mode, sweep speeds, and matching `--firmware-build-dir` so the reported firmware clock matches the flashed image.
7. Compare pass rate, mismatch position, sampler overruns, sink overruns, ring drops, stalls, and peak ring depth.
8. Record the run in `docs/testlog/rp2040-benchmark-testlog.md` using `docs/testlog/rp2040-benchmark-testlog-template.md`, including commands, measured results, and interpretation.
9. Compare the result against `docs/testlog/rp2040-benchmark-baseline.md` and report whether the measured SPI benchmark envelope changed compared with the previous baseline, including any regression or improvement in pass point, throughput, or overrun behavior.
10. Only update `docs/testlog/rp2040-benchmark-baseline.md` when the user explicitly asks for a baseline update.

## Core Commands
- Focused firmware tests: `cmake --build build/tests --target usb_app_test && ./build/tests/usb_app_test`
- Build Pico firmware: `cmake --build build/firmware-pico --target picotrace`
- Flash Pico firmware: `./tools/linux/load.sh --board pico --firmware-build-dir build/firmware-pico --skip-build`
- Build default 250 MHz Pico firmware: `./tools/linux/build.sh --board pico --firmware-build-dir build/firmware-pico-250m --system-clock-khz 250000`
- Flash default 250 MHz Pico firmware: `./tools/linux/load.sh --board pico --firmware-build-dir build/firmware-pico-250m --skip-build`
- Build clock-specific Pico firmware: `./tools/linux/build.sh --board pico --firmware-build-dir build/firmware-pico-225m --system-clock-khz 225000`
- Flash clock-specific Pico firmware: `./tools/linux/load.sh --board pico --firmware-build-dir build/firmware-pico-225m --skip-build`
- Build Pico 2 firmware: `cmake --build build/firmware-pico2 --target picotrace`
- Flash Pico 2 firmware: `./tools/linux/load.sh --board pico2 --firmware-build-dir build/firmware-pico2 --skip-build`
- Default 250 MHz MOSI sweep: `./.venv/bin/python tools/linux/spi_trace_benchmark.py --board pico --firmware-build-dir build/firmware-pico-250m --capture mosi --speed-hz 15500000 16000000 16500000 17000000 17500000 18000000 --trials 3`
- Default 250 MHz MOSI+MISO sweep: `./.venv/bin/python tools/linux/spi_trace_benchmark.py --board pico --firmware-build-dir build/firmware-pico-250m --capture mosi-miso --speed-hz 5600000 5700000 5800000 5900000 --trials 3`
- MOSI sweep: `./.venv/bin/python tools/linux/spi_trace_benchmark.py --board pico --firmware-build-dir build/firmware-pico-225m --capture mosi --speed-hz 11500000 12000000 12250000 12500000 --trials 3`
- MOSI+MISO sweep: `./.venv/bin/python tools/linux/spi_trace_benchmark.py --board pico --firmware-build-dir build/firmware-pico-225m --capture mosi-miso --speed-hz 4500000 5000000 --trials 3`

## Interpretation
- `sampler > 0` means the sampler or DMA service path is falling behind.
- `sink > 0` with `sampler = 0` means the downstream ring or USB drain path is the bottleneck.
- `peak=256` indicates the trace ring filled completely.
- High `stalls` without loss can still be acceptable if pass rate remains clean and `sink=0 sampler=0 ring=0`.

## References
- Benchmark commands and update checklist: [benchmark-reference](./references/benchmark-reference.md)
- RP2040 baseline page: `docs/testlog/rp2040-benchmark-baseline.md`
- RP2040 dated run log: `docs/testlog/rp2040-benchmark-testlog.md`
- RP2040 log template: `docs/testlog/rp2040-benchmark-testlog-template.md`
