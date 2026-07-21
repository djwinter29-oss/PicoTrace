# RP2040 Benchmark Baseline

This page is the stable RP2040 reference for SPI throughput and I2C trace validation on Raspberry Pi Pico.

Use [rp2040-benchmark-testlog.md](rp2040-benchmark-testlog.md) for dated runs, regressions, and investigation notes.
Only update this page when the baseline itself changes.

## Current Baseline

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
| `18.0 MHz` | `3/3` | Current highest verified lossless pass point |

Observed throughput across the verified MOSI pass points was about `7.4-8.5 Mb/s`.

### MOSI + MISO At 250 MHz

| Requested SPI clock | Pass rate | Notes |
| --- | --- | --- |
| `5.4 MHz` | `3/3` | Clean pass with zero sampler and sink overruns |
| `5.5 MHz` | `3/3` | Clean pass with zero sampler and sink overruns |
| `5.6 MHz` | `3/3` | Clean pass with zero sampler and sink overruns |
| `5.7 MHz` | `5/5` | Clean pass with zero sampler and sink overruns |
| `5.75 MHz` | `5/5` | Clean pass with zero sampler and sink overruns |
| `5.8 MHz` | `5/5` | Current highest verified lossless pass point |
| `5.9 MHz` | `2/5` | First unstable downstream-limited point; failing trials hit `sink=ring` and `peak=256` |
| `6.0 MHz` | `1/3` | Mostly downstream-limited failures |

Observed throughput across the verified MOSI+MISO pass points was about `3.2-3.6 Mb/s`.

## Current I2C Reference

- traffic source: `i2cdetect -y 1`
- logical channel: `0`
- firmware rate model: any non-zero `sample-hz` is accepted; the PIO clock divider is clamped to `1.0`, so there is no lower firmware-enforced ceiling below `clk_sys`
- helper: `./.venv/bin/python tools/linux/i2c_trace_test.py --channel 0 --bus 1 --sample-hz 4000000 --expect-transactions 112`
- expected transactions: `112`
- expected starts: `112`
- expected stops: `112`
- expected monitor overruns: `0`
- expected sticky error state: `0`
- current conservative smoke-test rate: `4000000`
- current highest clean RP2040 bench rate for this workload: `6600000`
- first unstable RP2040 bench rate for this workload: `6650000` (`overruns=10 sticky=1`)
- clearly failing RP2040 bench rates for this workload: `6750000` failed to stay running; `8000000` overran heavily and only captured `79/112` transactions

Interpretation:

- `transactions=112` means the RP2040 bench captured the expected Linux address-probe workload.
- balanced `starts` and `stops` mean the I2C event stream stayed complete.
- `overruns=0` and `sticky=0` mean the I2C monitor stayed healthy.
- for the current RP2040 bench, treat `6600000` as the practical upper clean point for the standard `i2cdetect` smoke workload.

## Historical Comparison

| Firmware clock | MOSI clean point | MOSI+MISO clean point | Notes |
| --- | --- | --- | --- |
| `250 MHz` | `18.0 MHz` | `5.8 MHz` | Current baseline |
| `225 MHz` | `15.5 MHz` | `5.4 MHz` | Previous baseline after packet-size and clock work |
| `200 MHz` | `11.5 MHz` | below `5.0 MHz` | Older baseline; dual-lane `5.0 MHz` was not reliable |

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
  --capture mosi \
  --speed-hz 15500000 16000000 16500000 17000000 17500000 18000000
```

Run a MOSI+MISO edge sweep:

```bash
./.venv/bin/python tools/linux/spi_trace_benchmark.py \
  --board pico \
  --capture mosi-miso \
  --speed-hz 5600000 5700000 5750000 5800000 5900000 6000000
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
for hz in 4000000 5000000 6000000 6250000 6500000 6600000 6650000 6750000 7000000 8000000; do
  ./tools/linux/load.sh --board pico --firmware-build-dir build/firmware-pico --skip-build >/dev/null || break
  printf '\n=== %s ===\n' "$hz"
  ./.venv/bin/python tools/linux/i2c_trace_test.py \
    --channel 0 \
    --bus 1 \
    --sample-hz "$hz" \
    --expect-transactions 112 | tail -n 3
done
```
