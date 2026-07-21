# Benchmark Reference

## Current Pico RP2040 Reference

- Current Pico firmware clock baseline: `225 MHz`
- Historical Pico firmware clock baseline retained in docs: `200 MHz`
- Current baseline document: `docs/testlog/rp2040-benchmark-baseline.md`
- Current dated run log: `docs/testlog/rp2040-benchmark-testlog.md`
- Current log template: `docs/testlog/rp2040-benchmark-testlog-template.md`

## Standard Validation Sequence

1. `cmake --build build/tests --target usb_app_test && ./build/tests/usb_app_test`
2. `cmake --build build/firmware-pico --target picotrace`
3. `./tools/linux/load.sh --board pico --firmware-build-dir build/firmware-pico --skip-build`
4. `source .venv/bin/activate`
5. Run one or more benchmark sweeps with `tools/linux/spi_trace_benchmark.py`

## Standard Pico Sweeps

### MOSI

```bash
./.venv/bin/python tools/linux/spi_trace_benchmark.py \
  --board pico \
  --capture mosi \
  --speed-hz 11500000 12000000 12250000 12500000 \
  --trials 3
```

### MOSI + MISO

```bash
./.venv/bin/python tools/linux/spi_trace_benchmark.py \
  --board pico \
  --capture mosi-miso \
  --speed-hz 4500000 5000000 \
  --trials 3
```

## Reporting Checklist

- Record the run first in `docs/testlog/rp2040-benchmark-testlog.md`
- Record board family and firmware clock
- Record requested speed list and trial count
- Record pass rate at each speed
- Record whether failures were sampler-limited or sink-limited
- Preserve older baselines when updating `docs/testlog/rp2040-benchmark-baseline.md`
