# RP2040 Benchmark Baseline

This page is the stable RP2040 reference for SPI throughput and I2C trace validation on Raspberry Pi Pico.

Use [rp2040-benchmark-testlog.md](rp2040-benchmark-testlog.md) for dated runs, regressions, and investigation notes.
Only update this page when the baseline itself changes.

## Current 250 MHz Baseline

- board: Raspberry Pi Pico (`RP2040`)
- firmware clock: `250 MHz`
- trace packet size: `896` bytes
- trace payload bytes: `880` bytes
- trace ring capacity: `256` packets
- stream service passes: `16`
- flush policy: flush vendor bulk only after a stream pass made progress
- host reader: threaded bulk reader with `65536`-byte USB reads

## Standard Workload

Unless a run says otherwise, compare against this workload:

- SPI device: `/dev/spidev0.0`
- SPI mode: `0`
- bits per word: `8`
- timeout: `20000 us`
- chunk bytes: `3968`
- repeat count: `240`
- total transfer bytes per trial: `952320`
- warmup bytes: `256`
- marker bytes: fixed `64`-byte marker block before the measured window
- drain wait after transmit: `1.5 s`
- trials per speed: `3`

## Current SPI Reference

### MOSI Only At 250 MHz

| Requested SPI clock | Pass rate | Notes |
| --- | --- | --- |
| `15.5 MHz` | `3/3` | Clean payload pass; one trial reported a single sampler overrun increment |
| `16.0 MHz` | `3/3` | Clean pass with zero sampler and sink overruns |
| `16.5 MHz` | `3/3` | Clean pass with zero sampler and sink overruns |
| `17.0 MHz` | `3/3` | Clean pass with zero sampler and sink overruns |
| `17.5 MHz` | `3/3` | Clean pass with zero sampler and sink overruns |
| `18.0 MHz` | `3/3` | Clean pass with zero sampler and sink overruns |
| `18.5 MHz` | `3/3` | Current highest verified lossless pass point |

Observed throughput across the verified MOSI pass points was about `7.4-8.5 Mb/s`.

### MOSI + MISO At 250 MHz

| Requested SPI clock | Pass rate | Notes |
| --- | --- | --- |
| `5.4 MHz` | `3/3` | Clean pass with zero sampler and sink overruns |
| `5.5 MHz` | `3/3` | Clean pass with zero sampler and sink overruns |
| `5.6 MHz` | `3/3` | Clean pass with zero sampler and sink overruns |
| `5.7 MHz` | `3/3` | Clean pass with zero sampler and sink overruns |
| `5.75 MHz` | `3/3` | Clean pass with zero sampler and sink overruns |
| `5.8 MHz` | `3/3` | Current highest verified lossless pass point |
| `5.9 MHz` | `1/3` | First unstable downstream-limited point; failing trials hit `sink=ring` and `peak=256` |
| `6.0 MHz` | `2/3` | Still unstable and downstream-limited |

Observed throughput across the verified MOSI+MISO pass points was about `3.2-3.6 Mb/s`.

## Current I2C Reference

| Field | Value |
| --- | --- |
| Traffic source | `i2cdetect -y 1` |
| Logical channel | `0` |
| Conservative smoke-test rate | `4000000` |
| Highest clean `250 MHz` sample-hz | `6600000` |
| First unstable `250 MHz` sample-hz | `6650000` with `overruns=13 sticky=1` and `110/112` transactions |
| Clearly failing `250 MHz` sample-hz | `6750000` produced heavy overruns; `8000000` failed to re-enter a healthy running state |
| Expected healthy result | `transactions=112 starts=112 stops=112 overruns=0 sticky=0` |
| Firmware rate model | Any non-zero `sample-hz` is accepted; the PIO clock divider is clamped to `1.0` |
| Helper | `./.venv/bin/python tools/linux/i2c_trace_test.py --channel 0 --bus 1 --sample-hz 4000000 --expect-transactions 112` |

Interpretation:

- `transactions=112` means the RP2040 bench captured the expected Linux address-probe workload.
- balanced `starts` and `stops` mean the I2C event stream stayed complete.
- `overruns=0` and `sticky=0` mean the I2C monitor stayed healthy.
- for the current `250 MHz` RP2040 bench, treat `6600000` as the practical upper clean point for the standard `i2cdetect` smoke workload.

## Cross-Clock Reference

Measured on 2026-07-21 with clock-specific RP2040 builds flashed through `tools/linux/load.sh`.

### SPI MOSI Across Firmware Clocks

| Firmware clock | Highest clean SPI clock | First unstable point | Notes |
| --- | --- | --- | --- |
| `150 MHz` | `15.5 MHz` | not reached in current sweep | `14.5-15.5 MHz` all passed `3/3`; higher edge not yet swept |
| `200 MHz` | `15.0 MHz` | not reached in current sweep | `13.0-15.0 MHz` all passed `3/3`; higher edge not yet swept |
| `225 MHz` | `17.5 MHz` | `18.0 MHz` (`2/3`) | `18.5 MHz` failed `0/3` |
| `250 MHz` | `18.5 MHz` | `19.0 MHz` (`0/3`) | Current best verified RP2040 MOSI point |

### SPI MOSI+MISO Across Firmware Clocks

| Firmware clock | Highest clean SPI clock | First unstable point | Notes |
| --- | --- | --- | --- |
| `150 MHz` | `5.2 MHz` | `5.3 MHz` (`1/3`) | `5.4 MHz` also fell to `1/3` |
| `200 MHz` | `5.8 MHz` | `5.9 MHz` (`2/3`) | `6.0 MHz` fell to `1/3` |
| `225 MHz` | `5.6 MHz` | `5.7 MHz` (`1/3`) | `5.75 MHz` improved to `2/3` but was still unstable |
| `250 MHz` | `5.8 MHz` | `5.9 MHz` (`1/3`) | `6.0 MHz` remained unstable at `2/3` |

### I2C Across Firmware Clocks

| Firmware clock | Highest clean `sample-hz` | First unstable point | Notes |
| --- | --- | --- | --- |
| `150 MHz` | `3.75 MHz` | `4.0 MHz` | `4.0 MHz` still captured `112` transactions but set `overruns=104 sticky=1` |
| `200 MHz` | `5.25 MHz` | `5.5 MHz` | `5.5 MHz` frequently failed to re-enter a healthy running state |
| `225 MHz` | `5.75 MHz` | `6.0 MHz` | `6.0 MHz` captured `112` transactions but set `overruns=141 sticky=1` |
| `250 MHz` | `6.6 MHz` | `6.65 MHz` | `6.65 MHz` dropped to `110/112` with `overruns=13 sticky=1` |

Read the I2C table as the highest rate that stayed healthy on the standard `i2cdetect -y 1` workload without setting monitor overrun or sticky error state.

## Failure Signatures

- `sampler=0 sink=ring peak=256`: downstream ring or USB drain bottleneck
- rising `sampler`: sampler or DMA service path is falling behind
- high `stalls` with `sink=0 sampler=0 ring=0`: backpressure is present but still lossless at that point

## Standard Commands

Create the host virtual environment if needed:

```bash
./tools/linux/host_python_venv.sh
```

Run a MOSI-only edge sweep:

```bash
./.venv/bin/python tools/linux/spi_trace_benchmark.py \
  --board pico \
  --firmware-build-dir build/firmware-pico-250m \
  --capture mosi \
  --speed-hz 15500000 16000000 16500000 17000000 17500000 18000000
```

Run a MOSI+MISO edge sweep:

```bash
./.venv/bin/python tools/linux/spi_trace_benchmark.py \
  --board pico \
  --firmware-build-dir build/firmware-pico-250m \
  --capture mosi-miso \
  --speed-hz 5600000 5700000 5750000 5800000 5900000 6000000
```

Build and flash one clock-specific RP2040 image:

```bash
./tools/linux/build.sh \
  --board pico \
  --firmware-build-dir build/firmware-pico-225m \
  --system-clock-khz 225000

./tools/linux/load.sh \
  --board pico \
  --firmware-build-dir build/firmware-pico-225m \
  --skip-build
```

Run the conservative I2C smoke check:

```bash
./.venv/bin/python tools/linux/i2c_trace_test.py \
  --channel 0 \
  --bus 1 \
  --sample-hz 4000000 \
  --expect-transactions 112
```

Sweep I2C sample-hz to determine the current clean point for the standard smoke workload:

```bash
build_dir=build/firmware-pico-250m

for hz in 4000000 5000000 6000000 6250000 6500000 6600000 6650000 6750000 7000000 8000000; do
  ./tools/linux/load.sh --board pico --firmware-build-dir "$build_dir" --skip-build >/dev/null || break
  printf '\n=== %s ===\n' "$hz"
  ./.venv/bin/python tools/linux/i2c_trace_test.py \
    --channel 0 \
    --bus 1 \
    --sample-hz "$hz" \
    --expect-transactions 112 | tail -n 3
done
```
