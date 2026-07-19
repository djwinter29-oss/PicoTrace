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
- Reproduce the documented RP2040 benchmark sweeps
- Update benchmark documentation with fresh measured results

## Prerequisites
- Work in the PicoTrace repository root
- Use the bench wiring from `docs/raspberry-pi-test-setup.md`
- Ensure `/dev/spidev0.0` exists on the Raspberry Pi host
- Ensure the PicoTrace board enumerates over USB
- Activate the project virtual environment when running the Python benchmark script

## Procedure
1. Check the current benchmark notes in `docs/rp2040-benchmark.md` before changing the measured baseline.
2. Rebuild focused firmware tests when the SPI trace path changed.
3. Rebuild and flash the target firmware before benchmarking.
4. Run the SPI benchmark script with the correct board, capture mode, and sweep speeds.
5. Compare pass rate, mismatch position, sampler overruns, sink overruns, ring drops, stalls, and peak ring depth.
6. If the benchmark envelope changed, update `docs/rp2040-benchmark.md` and preserve older baselines as historical reference.

## Core Commands
- Focused firmware tests: `cmake --build build/tests --target usb_app_test && ./build/tests/usb_app_test`
- Build Pico firmware: `cmake --build build/firmware-pico --target picotrace`
- Flash Pico firmware: `./tools/linux/load.sh --board pico --firmware-build-dir build/firmware-pico --skip-build`
- Build Pico 2 firmware: `cmake --build build/firmware-pico2 --target picotrace`
- Flash Pico 2 firmware: `./tools/linux/load.sh --board pico2 --firmware-build-dir build/firmware-pico2 --skip-build`
- MOSI sweep: `./.venv/bin/python tools/linux/spi_trace_benchmark.py --board pico --capture mosi --speed-hz 11500000 12000000 12250000 12500000 --trials 3`
- MOSI+MISO sweep: `./.venv/bin/python tools/linux/spi_trace_benchmark.py --board pico --capture mosi-miso --speed-hz 4500000 5000000 --trials 3`

## Interpretation
- `sampler > 0` means the sampler or DMA service path is falling behind.
- `sink > 0` with `sampler = 0` means the downstream ring or USB drain path is the bottleneck.
- `peak=256` indicates the trace ring filled completely.
- High `stalls` without loss can still be acceptable if pass rate remains clean and `sink=0 sampler=0 ring=0`.

## References
- Benchmark commands and update checklist: [benchmark-reference](./references/benchmark-reference.md)
