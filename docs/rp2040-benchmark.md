# RP2040 Benchmark Notes

This document captures the current sustained-throughput benchmark shape for the Raspberry Pi Pico
(`RP2040`) firmware and the most recent measured results on the current `200 MHz` baseline.

## What This Benchmark Measures

The benchmark drives a long SPI transmit window from the Raspberry Pi host into one observed bus,
reads back the PicoTrace bulk stream, and checks both payload integrity and device-side session
counters.

The current benchmark is intentionally end-to-end:

- the host configures SPI capture through the HID control path
- the firmware captures and packetizes SPI traffic into the trace ring
- Core 0 drains the ring to the TinyUSB vendor bulk endpoint
- the Python host drains and decodes the bulk stream

This means the result is a throughput ceiling for the full stack, not just the sampler.

## Current RP2040 Baseline

The current `RP2040` benchmark baseline in this repository is:

- board: Raspberry Pi Pico
- firmware clock: `200 MHz`
- trace packet size: `512` bytes
- trace payload bytes: `496` bytes
- trace ring capacity: `256` packets
- stream service passes: `16`
- flush policy: flush vendor bulk only after a stream pass made progress
- host reader: threaded bulk reader with `65536`-byte USB reads

## Workload Shape

Unless a run says otherwise, the benchmark uses this transfer window:

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

## Current Results

These are the current reference results on the `200 MHz` RP2040 baseline.

### MOSI Only

| Requested SPI clock | Pass rate | Notes |
| --- | --- | --- |
| `11.5 MHz` | `3/3` | Current reliable sustained ceiling |
| `12.0 MHz` | `1/3` | First unstable point |
| `12.25 MHz` | `0/3` | Consistent failures |
| `12.5 MHz` | `0/3` | Consistent failures |

Observed transmit throughput at the reliable `11.5 MHz` point was about `6.5-6.6 Mb/s` over the
full transfer window.

### MOSI + MISO

| Requested SPI clock | Pass rate | Notes |
| --- | --- | --- |
| `4.5 MHz` | `2/3` | Close to stable, but not fully deterministic |
| `5.0 MHz` | `1/3` | Not reliable on the `200 MHz` baseline |

## How To Interpret Failures

The benchmark prints the firmware-side session counters after every trial.

The common current failure signature is:

- `sampler=0`
- `sink=ring`
- `peak=256`

That pattern means the sampler kept up, but the downstream path filled the entire trace ring and
the sink side dropped packets before the host drained them fast enough.

If `sampler` starts climbing, the failure mode has moved earlier in the pipeline and the result is
no longer purely a USB drain or host drain limit.

## Running The Benchmark

Create the host virtual environment first if needed:

```bash
./tools/linux/host_python_venv.sh
```

Run a MOSI-only edge sweep on the current RP2040 baseline:

```bash
./.venv/bin/python tools/linux/spi_trace_benchmark.py \
  --capture mosi \
  --speed-hz 11500000 12000000 12250000 12500000
```

Run a dual-line sweep:

```bash
./.venv/bin/python tools/linux/spi_trace_benchmark.py \
  --capture mosi-miso \
  --speed-hz 4500000 5000000
```

The script prints one summary line per requested speed and includes:

- pass rate
- captured byte count
- first mismatch index when a trial fails
- measured transmit throughput in `Mb/s`
- transfer window duration
- emitted packet count
- firmware-side overrun, ring-drop, stall, and peak-ring counters

## Notes

- The benchmark assumes the SPI traffic generator is the Linux host on the same Raspberry Pi that
  runs the script.
- The benchmark does not rebuild or flash firmware; run the normal build/load flow first when you
  need to benchmark a new firmware revision.
- Because the edge is narrow, compare multiple trials instead of trusting a single pass or fail.
