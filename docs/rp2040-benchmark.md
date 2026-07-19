# RP2040 Benchmark Notes

This document captures the current sustained-throughput benchmark shape for the Raspberry Pi Pico
(`RP2040`) firmware and the most recent measured results on the current `250 MHz` baseline.

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
- firmware clock: `250 MHz`
- trace packet size: `896` bytes
- trace payload bytes: `880` bytes
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

These are the current reference results on the `250 MHz` RP2040 baseline.

### MOSI Only At 250 MHz

| Requested SPI clock | Pass rate | Notes |
| --- | --- | --- |
| `15.5 MHz` | `3/3` | Clean payload pass; one trial reported a single sampler overrun counter increment |
| `16.0 MHz` | `3/3` | Clean pass with zero sampler and sink overruns |
| `16.5 MHz` | `3/3` | Current highest verified lossless pass point |
| `17.0 MHz` | `1/3` | First unstable downstream-limited point; failing trials hit `sink=ring` and `peak=256` |
| `17.5 MHz` | `0/3` | Consistent downstream-limited failures |
| `18.0 MHz` | `0/3` | Failure mode moves upstream; sampler overruns dominate |

Observed transmit throughput across the verified MOSI pass points at `250 MHz` was about
`7.6-8.1 Mb/s` over the full transfer window.

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

Observed transmit throughput across the verified MOSI+MISO pass points at `250 MHz` was about
`3.2-3.6 Mb/s` over the full transfer window.

## Historical 225 MHz Reference

Before the current `250 MHz` Pico baseline, this repository tracked the following `225 MHz`
RP2040 reference results.

## Historical 200 MHz Reference

Before the earlier `225 MHz` Pico baseline, this repository tracked the following `200 MHz`
RP2040 reference results.

These older numbers remain useful when comparing the effect of later sampler, packetization, or
clock changes against the previous sustained-throughput envelope.

### MOSI Only At 200 MHz

| Requested SPI clock | Pass rate | Notes |
| --- | --- | --- |
| `11.5 MHz` | `3/3` | Previous reliable sustained ceiling |
| `12.0 MHz` | `1/3` | First unstable point on the older baseline |
| `12.25 MHz` | `0/3` | Consistent failures |
| `12.5 MHz` | `0/3` | Consistent failures |

Observed transmit throughput at the reliable `11.5 MHz` point was about `6.5-6.6 Mb/s` over the
full transfer window.

### MOSI + MISO At 200 MHz

| Requested SPI clock | Pass rate | Notes |
| --- | --- | --- |
| `4.5 MHz` | `2/3` | Close to stable, but not fully deterministic |
| `5.0 MHz` | `1/3` | Not reliable on the older baseline |

### MOSI Only

| Requested SPI clock | Pass rate | Notes |
| --- | --- | --- |
| `11.5 MHz` | `3/3` | Clean pass with zero sampler and sink overruns |
| `12.0 MHz` | `3/3` | Clean pass with zero sampler and sink overruns |
| `12.25 MHz` | `3/3` | Clean pass with zero sampler and sink overruns |
| `12.5 MHz` | `3/3` | Clean pass with zero sampler and sink overruns |
| `12.75 MHz` | `3/3` | Clean pass with zero sampler and sink overruns |
| `13.0 MHz` | `3/3` | Clean pass with zero sampler and sink overruns |
| `13.25 MHz` | `3/3` | Clean pass with zero sampler and sink overruns |
| `13.5 MHz` | `3/3` | Clean pass with zero sampler and sink overruns |
| `14.0 MHz` | `3/3` | Clean pass after increasing trace packet size to `896` bytes |
| `14.5 MHz` | `3/3` | Clean pass with zero sampler and sink overruns |
| `15.0 MHz` | `3/3` | Clean pass with zero sampler and sink overruns |
| `15.5 MHz` | `3/3` | Current highest verified lossless pass point |

Observed transmit throughput across the verified MOSI pass range was about `6.8-7.9 Mb/s` over
the full transfer window.

### MOSI + MISO

| Requested SPI clock | Pass rate | Notes |
| --- | --- | --- |
| `4.5 MHz` | `3/3` | Clean pass with zero sampler and sink overruns |
| `5.0 MHz` | `3/3` | Clean pass with zero sampler and sink overruns |
| `5.1 MHz` | `3/3` | Clean pass with zero sampler and sink overruns |
| `5.2 MHz` | `3/3` | Clean pass after increasing trace packet size to `896` bytes |
| `5.25 MHz` | `3/3` | Clean pass with zero sampler and sink overruns |
| `5.3 MHz` | `3/3` | Clean pass with zero sampler and sink overruns |
| `5.4 MHz` | `3/3` | Current highest verified lossless pass point |

Observed transmit throughput across the verified MOSI+MISO pass range was about `2.9-3.6 Mb/s`
over the full transfer window.

## How To Interpret Failures

The benchmark prints the firmware-side session counters after every trial.

The common downstream-limit failure signature is:

- `sampler=0`
- `sink=ring`
- `peak=256`

That pattern means the sampler kept up, but the downstream path filled the entire trace ring and
the sink side dropped packets before the host drained them fast enough.

If `sampler` starts climbing, the failure mode has moved earlier in the pipeline and the result is
no longer purely a USB drain or host drain limit.

At the current `250 MHz` reference points listed above, the verified passing runs stayed lossless,
except for one `15.5 MHz` MOSI trial that reported a single sampler overrun count without payload
loss:

- `sampler=0`
- `sink=0`
- `ring=0`

USB stall counts can still be high near the edge, and peak ring depth can rise substantially, but
those signals did not produce data loss at the documented passing points.

The current edge behavior on this `250 MHz` baseline improved further after raising the RP2040
system clock from `225 MHz` to `250 MHz` while keeping fixed trace packet size at `896` bytes:

- MOSI-only capture remains lossless through `16.5 MHz`, becomes unstable at `17.0 MHz`, and
  then fails consistently by `17.5 MHz`; the `18.0 MHz` failure mode shifts upstream into sampler
  overruns
- MOSI+MISO capture remains lossless through `5.8 MHz` and becomes unstable at `5.9 MHz` with the
  same downstream ring-full signature seen at lower clocks

## Running The Benchmark

Create the host virtual environment first if needed:

```bash
./tools/linux/host_python_venv.sh
```

Run a MOSI-only edge sweep on the current RP2040 baseline:

```bash
./.venv/bin/python tools/linux/spi_trace_benchmark.py \
  --board pico \
  --capture mosi \
  --speed-hz 15500000 16000000 16500000 17000000 17500000 18000000
```

Run a dual-line sweep:

```bash
./.venv/bin/python tools/linux/spi_trace_benchmark.py \
  --board pico \
  --capture mosi-miso \
  --speed-hz 5600000 5700000 5750000 5800000 5900000 6000000
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
- The benchmark script accepts `--board pico|pico2` so the reported firmware clock matches the
  target family under test.
- Because the edge is narrow, compare multiple trials instead of trusting a single pass or fail.
