# RP2040 Benchmark Notes

This document captures the current sustained-throughput benchmark shape for the Raspberry Pi Pico
(`RP2040`) firmware, the most recent measured SPI results on the current `250 MHz` baseline, and
the current RP2040 I2C trace-validation baseline used for later comparison.

## Report Page Use

Use this page as the canonical RP2040 comparison report whenever firmware changes may affect SPI or
I2C trace behavior.

Keep the current best-known RP2040 results here so later firmware work can compare against one
stable reference page.

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
| `16.5 MHz` | `3/3` | Clean pass with zero sampler and sink overruns |
| `17.0 MHz` | `3/3` | Current highest verified lossless pass point after moving SPI to `pio0` and I2C to `pio1` |
| `17.5 MHz` | `1/3` | First unstable downstream-limited point; failing trials hit `sink=ring` and `peak=256` |
| `18.0 MHz` | `0/3` | Failure mode moves upstream; sampler overruns dominate |

Observed transmit throughput across the verified MOSI pass points at `250 MHz` was about
`7.4-8.0 Mb/s` over the full transfer window.

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

## Recent Investigation Notes

These notes capture newer measurements taken while investigating the SPI test-hook cleanup on
`2026-07-21`.

They are recorded here because the measured envelope changed, but they do not replace the
best-known `250 MHz` baseline above until the regression cause is fixed or the new result is shown
to be stable and intentional.

### RP2040 SPI Regression Probe On 2026-07-21

Focused commands used during the investigation:

- `./tools/linux/load.sh --board pico`
- `./.venv/bin/python tools/linux/spi_trace_benchmark.py --board pico --capture mosi --speed-hz 17000000 17500000 18000000 --trials 3`
- `./.venv/bin/python tools/linux/spi_trace_benchmark.py --board pico --capture mosi-miso --speed-hz 5800000 5900000 --trials 3`

Observed results on the investigation build:

- MOSI `17.0 MHz`: `0/3` on the first run after the SPI test-hook refactor, with mixed failure signatures including small sampler overruns and downstream ring-full loss
- MOSI `17.5 MHz`: `0/3`
- MOSI `18.0 MHz`: `0/3`, still dominated by sampler overruns
- MOSI+MISO `5.8 MHz`: `3/3`
- MOSI+MISO `5.9 MHz`: `1/3`, still in the same unstable edge region as the established baseline

Additional probe results during the same investigation:

- moving the new SPI test-only wrappers out of the production firmware image improved MOSI `17.0 MHz` from `0/3` to `1/3`
- restoring one removed dead helper for a code-layout probe pushed MOSI `17.0 MHz` back to `0/3`

Current interpretation:

- the recent regression is most likely layout-sensitive on RP2040 XIP flash rather than a direct logic bug in the active SPI DMA poll path
- the first known-good reference for comparison remains the baseline above: MOSI lossless through `17.0 MHz` and MOSI+MISO lossless through `5.8 MHz`
- until a follow-up benchmark re-establishes the lossless `17.0 MHz` MOSI point on the cleaned source tree, treat the current refactor branch as performance-regressed

### RP2040 USB Bulk Consume-Path Probe On 2026-07-21

Focused commands used after the `usb_bulk.c` consume-path cleanup:

- `./tools/linux/load.sh --board pico --firmware-build-dir build/firmware-pico --skip-build`
- `./.venv/bin/python tools/linux/spi_trace_benchmark.py --board pico --capture mosi --speed-hz 17000000 --trials 3`
- `./.venv/bin/python tools/linux/spi_trace_benchmark.py --board pico --capture mosi-miso --speed-hz 5800000 --trials 3`
- `./.venv/bin/python tools/linux/i2c_trace_test.py --channel 0 --bus 1 --sample-hz 4000000 --expect-transactions 112`

Observed results on the cleanup build:

- MOSI `17.0 MHz`: `3/3`, still lossless; one passing trial reported `sampler=1`, while downstream counters stayed at `sink=0 ring=0`
- MOSI+MISO `5.8 MHz`: `2/3`; the earlier failing trial was downstream-limited with `sink=37 sampler=0 ring=37 stalls=491918 peak=256`
- MOSI+MISO `5.8 MHz` after folding stream flush policy into `usb_bulk_service_stream()`: still `2/3`; the failing trial remained downstream-limited with `sink=9 sampler=0 ring=9 stalls=523139 peak=256`
- MOSI `17.0 MHz` after adding host-backpressure versus policy-deferral counters and the whole-packet fast path: `3/3`, with `sink=0 sampler=0 ring=0`, `host_stalls` around `227k-255k`, and `policy_deferrals=0`
- MOSI+MISO `5.8 MHz` after the same cleanup: `3/3`, with `sink=0 sampler=0 ring=0`, `host_stalls` around `511k-513k`, and `policy_deferrals=0`
- I2C smoke test after the same cleanup stayed at `112` transactions with balanced `starts=112`, `stops=112`, `overruns=0`, and `sticky=0`

Current interpretation:

- the consume-path cleanup did not reduce the current MOSI-only pass point; `17.0 MHz` remains clean enough to keep the existing baseline unchanged
- MOSI+MISO at `5.8 MHz` remains on the unstable edge on the current branch even though the best-known baseline still records `5/5`
- the failing `5.8 MHz` trial is still downstream-limited, so the active suspicion remains ring or USB-drain pressure rather than sampler loss
- folding stream flush policy into `usb_bulk_service_stream()` simplified ownership, but it did not measurably move the current MOSI+MISO unstable edge on RP2040
- the new split counters show that the observed downstream pressure in the latest clean runs is entirely host-backpressure-driven on the vendor endpoint; the stream policy itself reported `policy_deferrals=0`
- the whole-packet fast path plus counter split coincided with a clean `3/3` recheck at MOSI+MISO `5.8 MHz`, so the current branch no longer shows the earlier downstream-limited failure at that point

## Current I2C Trace Baseline

These are the current reference expectations for the Raspberry Pi Pico (`RP2040`) I2C trace smoke
test on the bench wiring described in `docs/raspberry-pi-test-setup.md`.

- traffic source: `i2cdetect -y 1`
- PicoTrace logical channel: `0`
- current repo-local test helper: `./.venv/bin/python tools/linux/i2c_trace_test.py --channel 0 --bus 1 --sample-hz 4000000 --expect-transactions 112`
- expected transactions: `112`
- expected start events: `112`
- expected stop events: `112`
- expected monitor overruns: `0`
- expected sticky error state: `0`

Current interpretation:

- `112` traced transactions means the RP2040 bench captured the expected Linux address-probe workload.
- balanced `start` and `stop` counts mean the captured I2C event stream stayed complete for the scan.
- `overruns=0` and `sticky=0` mean the I2C monitor stayed healthy during the smoke test.
- the latest validation after moving I2C to `pio1` kept the same `112`-transaction smoke-test result with `overruns=0` and `sticky=0`
- the latest validation after the `usb_bulk.c` consume-path cleanup kept the same `112`-transaction smoke-test result with `overruns=0` and `sticky=0`

### Current I2C Stress Check

In addition to the smoke test above, the repository now carries a repeatable repeated-start stress
workload driven by Linux `i2ctransfer`:

- workload source: `./.venv/bin/python tools/linux/i2c_trace_test.py --channel 0 --bus 1 --sample-hz 4000000 --workload combined-burst --target-address 0x50 --read-length 4 --repeat-count 64 --expect-transactions 0`
- primary purpose: denser repeated-start traffic for decode and backlog validation
- comparison signal: repeated-start shape (`starts = 2 * stops`) plus `overruns=0` and `sticky=0`
- transaction-count matching is intentionally disabled by default because the target address may NACK while still producing observable traffic

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

- MOSI-only capture now remains lossless through `17.0 MHz`, becomes unstable at `17.5 MHz`, and
  then fails consistently by `18.0 MHz`; the `18.0 MHz` failure mode shifts upstream into sampler
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
- The I2C trace helper uses the board-local CDC `i2cmon` command plus passive USB bulk decode and
  is useful as a fast smoke test after firmware changes.
