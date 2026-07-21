# RP2040 Benchmark Test Log

This file is the dated RP2040 run log for benchmark and I2C trace-validation results.

Add new entries here after firmware-affecting test runs.

Use [rp2040-benchmark-testlog-template.md](rp2040-benchmark-testlog-template.md) for the entry format.

## 2026-07-21 - USB Bulk Consume Path Simplification Follow-Up

### Scope

- board: Raspberry Pi Pico (`RP2040`)
- firmware clock: `250 MHz`
- firmware/build: `build/firmware-pico-250m`
- reason: validate the USB bulk consume-path cleanup that flushes only after progress, allows packet-completing unaligned tails, caches vendor write availability per service pass, batches policy-deferral accounting, and folds packet validation into the stream-span preparation helper

### Commands

- `cmake --build build/tests --target usb_app_test && ./build/tests/usb_app_test`
- `./tools/linux/build.sh --board pico --firmware-build-dir build/firmware-pico-250m --system-clock-khz 250000`
- `./tools/linux/load.sh --board pico --firmware-build-dir build/firmware-pico-250m --skip-build`
- `./.venv/bin/python tools/linux/i2c_trace_test.py --channel 0 --bus 1 --sample-hz 4000000 --expect-transactions 112`
- `./.venv/bin/python tools/linux/spi_trace_benchmark.py --board pico --firmware-build-dir build/firmware-pico-250m --capture mosi --speed-hz 15500000 16000000 16500000 17000000 17500000 18000000 --trials 3`
- `./.venv/bin/python tools/linux/spi_trace_benchmark.py --board pico --firmware-build-dir build/firmware-pico-250m --capture mosi-miso --speed-hz 5600000 5700000 5800000 5900000 --trials 3`

### Results

- hosted tests: `usb_app_test` built and ran cleanly with the rewritten consume path, including the new whole-packet unaligned-tail coverage and the new flush-only-after-progress expectation.
- MOSI: `15.5/16.0/16.5/17.0/17.5 MHz` passed `3/3`; `18.0 MHz` failed `0/3`. Failing trials stayed downstream-limited with `sink>0 sampler=0 ring=0 peak=255`.
- MOSI+MISO: `5.6/5.7/5.8 MHz` passed `3/3`; `5.9 MHz` passed `1/3`. Failing trials stayed downstream-limited with `sink>0 sampler=0 ring=0 peak=255`.
- I2C smoke: the standard `4.0 MHz` `i2cdetect -y 1` trace stayed healthy with `transactions=112 starts=112 stops=112 overruns=0 sticky=0`.
- I2C sample-hz notes: this follow-up reran only the conservative smoke point because the change was isolated to the USB consumer.

### Interpretation

- The consume path is structurally simpler now: one span-preparation helper owns packet validation and borrow state, one chunk writer owns the alignment policy, and `usb_bulk_service_stream()` no longer flushes when no bytes were queued.
- Allowing whole-packet tails to finish untrimmed removed most alignment-policy churn in the benchmark output: `policy_deferrals` dropped to `0` on nearly every passing standard trial instead of showing steady-state increments.
- The stable hardware envelope did not improve beyond the current RP2040 baseline on this run. MOSI remained clean through `17.5 MHz` with `18.0 MHz` unstable, and MOSI+MISO remained clean through `5.8 MHz` with `5.9 MHz` unstable.
- The failure signature remained downstream-limited rather than sampler-limited, so this cleanup reduced consumer-path overhead and observability noise more than it changed the current throughput ceiling.

### Baseline Impact

- `docs/testlog/rp2040-benchmark-baseline.md` updated: no
- reason: the measured SPI envelope matches the current published baseline rather than moving it.

## 2026-07-21 - USB Bulk Unified Chunk Write Follow-Up

### Scope

- board: Raspberry Pi Pico (`RP2040`)
- firmware clock: `250 MHz`
- firmware/build: `build/firmware-pico-250m`
- reason: validate the USB bulk consume-path simplification that removes the split between exact packet writes and partial stream writes

### Commands

- `cmake --build build/tests --target usb_app_test && ./build/tests/usb_app_test`
- `./tools/linux/build.sh --board pico --firmware-build-dir build/firmware-pico-250m --system-clock-khz 250000`
- `./tools/linux/load.sh --board pico --firmware-build-dir build/firmware-pico-250m --skip-build`
- `./.venv/bin/python tools/linux/i2c_trace_test.py --channel 0 --bus 1 --sample-hz 4000000 --expect-transactions 112`
- `./.venv/bin/python tools/linux/spi_trace_benchmark.py --board pico --firmware-build-dir build/firmware-pico-250m --capture mosi --speed-hz 15500000 16000000 16500000 17000000 17500000 18000000 --trials 3`
- `./.venv/bin/python tools/linux/spi_trace_benchmark.py --board pico --firmware-build-dir build/firmware-pico-250m --capture mosi-miso --speed-hz 5600000 5700000 5800000 5900000 --trials 3`

### Results

- hosted tests: `usb_app_test` built and ran cleanly with the unified chunk writer.
- MOSI: `15.5/16.0/16.5/17.0/17.5/18.0 MHz` all passed `3/3` with `sink=0 sampler=0 ring=0` on every trial.
- MOSI+MISO: `5.6/5.7/5.8 MHz` all passed `3/3`; `5.9 MHz` passed `1/3`. Failing `5.9 MHz` trials stayed downstream-limited with `sink>0 sampler=0 ring=0 peak=255`.
- I2C smoke: the standard `4.0 MHz` `i2cdetect -y 1` trace stayed healthy with `transactions=112 starts=112 stops=112 overruns=0 sticky=0`.
- counter notes: the unified chunk path now reports small non-zero `policy_deferrals` even on successful SPI runs because whole-packet transfers naturally flow through the same alignment policy instead of a separate exact-write fast path.

### Interpretation

- Removing the split between exact packet writes and stream-chunk writes improved the `250 MHz` SPI benchmark envelope on this bench without disturbing the I2C smoke path.
- MOSI improved beyond the current published baseline by making `18.0 MHz` clean at `3/3`.
- MOSI+MISO recovered to match the current published baseline at `5.8 MHz` clean, while `5.9 MHz` remained the unstable edge.
- The remaining SPI failures are still downstream-limited rather than sampler-limited, but the unified write path reduced enough policy branching to improve throughput in practice.

### Baseline Impact

- `docs/testlog/rp2040-benchmark-baseline.md` updated: no
- reason: the run is promising, but the workflow keeps the stable baseline unchanged unless explicitly requested.

## 2026-07-21 - I2C Transition And Ready-Cursor Follow-Up

### Scope

- board: Raspberry Pi Pico (`RP2040`)
- firmware clock: `250 MHz`
- firmware/build: `build/firmware-pico-250m`
- reason: validate the I2C monitor refactor that removes the separate transition-interest mask and replaces the ready-buffer scan with a single next-ready cursor

### Commands

- `cmake --build build/tests --target i2c_monitor_runtime_test && ./build/tests/i2c_monitor_runtime_test`
- `cmake --build build/tests --target usb_app_test && ./build/tests/usb_app_test`
- `./tools/linux/build.sh --board pico --firmware-build-dir build/firmware-pico-250m --system-clock-khz 250000`
- `./tools/linux/load.sh --board pico --firmware-build-dir build/firmware-pico-250m --skip-build`
- `./.venv/bin/python tools/linux/i2c_trace_test.py --channel 0 --bus 1 --sample-hz 4000000 --expect-transactions 112`
- `./.venv/bin/python tools/linux/spi_trace_benchmark.py --board pico --firmware-build-dir build/firmware-pico-250m --capture mosi --speed-hz 15500000 16000000 16500000 17000000 17500000 18000000 --trials 3`
- `./.venv/bin/python tools/linux/spi_trace_benchmark.py --board pico --firmware-build-dir build/firmware-pico-250m --capture mosi-miso --speed-hz 5600000 5700000 5800000 5900000 --trials 3`

### Results

- hosted tests: `i2c_monitor_runtime_test` and `usb_app_test` built and ran cleanly after repairing the refactor onto a clean source base.
- MOSI: `15.5/16.0/16.5/17.5 MHz` passed `3/3`; `17.0 MHz` was noisy at `2/3`; `18.0 MHz` failed `0/3`. Every failing trial stayed downstream-limited with `sink>0 sampler=0 ring=0 peak=255`.
- MOSI+MISO: `5.6 MHz` passed `3/3`; `5.7 MHz` passed `2/3`; `5.8 MHz` passed `1/3`; `5.9 MHz` passed `0/3`. Every failing trial stayed downstream-limited with `sink>0 sampler=0 ring=0 peak=255`.
- I2C smoke: the standard `4.0 MHz` `i2cdetect -y 1` trace stayed healthy with `transactions=112 starts=112 stops=112 overruns=0 sticky=0`.
- I2C sample-hz notes: this follow-up reran only the conservative smoke point because the code change was structural and localized to the I2C monitor poll/completion bookkeeping.

### Interpretation

- The I2C transition-state collapse and ready-buffer cursor change preserved the expected hosted and hardware I2C behavior; the standard smoke test remains clean with no overrun signal.
- SPI did not improve as a side effect of this I2C-local refactor. MOSI still tops out at the existing `17.5 MHz` clean point with `18.0 MHz` unstable, while MOSI+MISO remains below the current published `5.8 MHz` clean baseline on this bench.
- The SPI failures remained downstream-limited rather than sampler-limited, so this change did not alter the previously observed USB/ring-side bottleneck signature.
- The non-monotonic `17.0 MHz` MOSI result looks like bench noise rather than a new stable edge because `17.5 MHz` still passed `3/3` immediately afterward.

### Baseline Impact

- `docs/testlog/rp2040-benchmark-baseline.md` updated: no
- reason: the I2C behavior is unchanged at the smoke point, and the SPI benchmark envelope does not justify a baseline change.

## 2026-07-21 - 250 MHz SPI And I2C Benchmark Rerun

### Scope

- board: Raspberry Pi Pico (`RP2040`)
- firmware clock: `250 MHz`
- firmware/build: `build/firmware-pico-250m`
- reason: rerun the standard `250 MHz` SPI benchmark sweeps and I2C trace bracket on the current firmware image

### Commands

- `./tools/linux/load.sh --board pico --firmware-build-dir build/firmware-pico-250m --skip-build`
- `./.venv/bin/python tools/linux/spi_trace_benchmark.py --board pico --firmware-build-dir build/firmware-pico-250m --capture mosi --speed-hz 15500000 16000000 16500000 17000000 17500000 18000000 --trials 3`
- `./.venv/bin/python tools/linux/spi_trace_benchmark.py --board pico --firmware-build-dir build/firmware-pico-250m --capture mosi-miso --speed-hz 5600000 5700000 5750000 5800000 5900000 6000000 --trials 3`
- `for hz in 4000000 10000000 11000000 12000000; do ./.venv/bin/python tools/linux/i2c_trace_test.py --channel 0 --bus 1 --sample-hz "$hz" --expect-transactions 112; done`

### Results

- MOSI: `15.5/16.0/16.5/17.0/17.5 MHz` all passed `3/3`; `18.0 MHz` failed `0/3`. Failing trials stayed downstream-limited with `sink>0 sampler=0 ring=0 peak=252`.
- MOSI+MISO: `5.6 MHz` passed `3/3`; `5.7 MHz` passed `2/3`; `5.75 MHz` passed `1/3`; `5.8 MHz` passed `1/3`; `5.9 MHz` passed `1/3`; `6.0 MHz` passed `2/3`. Failing trials stayed downstream-limited with `sink>0 sampler=0 ring=0 peak=252`.
- I2C smoke: `4.0 MHz` stayed healthy with `transactions=112 starts=112 stops=112 overruns=0 sticky=0`.
- I2C sample-hz notes: `10.0 MHz`, `11.0 MHz`, and `12.0 MHz` all stayed healthy on the standard `i2cdetect -y 1` workload with `transactions=112 starts=112 stops=112 overruns=0 sticky=0`.

### Interpretation

- MOSI-only SPI matches the current published `250 MHz` baseline on this rerun: `17.5 MHz` stayed the highest clean point and `18.0 MHz` stayed unstable.
- MOSI+MISO SPI is still materially worse than the current published baseline on this bench because the baseline clean range through `5.8 MHz` did not hold in this rerun.
- I2C remains stronger than the current published `250 MHz` baseline on the standard smoke workload because the prior unstable bracket at `11-12 MHz` stayed healthy again.
- The SPI failures remained downstream-limited rather than sampler-limited.

### Baseline Impact

- `docs/testlog/rp2040-benchmark-baseline.md` updated: no
- reason: this rerun still shows mixed movement versus the current baseline, and the workflow says to leave the stable baseline unchanged unless explicitly requested.

## 2026-07-21 - SPI Producer Follow-Up

### Scope

- board: Raspberry Pi Pico (`RP2040`)
- firmware clock: `250 MHz`
- firmware/build: `build/firmware-pico-250m`
- reason: validate the SPI producer follow-up changes that split the hot append kernels, require one idle stale poll before timeout close, add an early saturation backoff, and keep single-slot boundary handling deterministic

### Commands

- `cmake --build build/tests --target usb_app_test && ./build/tests/usb_app_test`
- `./tools/linux/build.sh --board pico --firmware-build-dir build/firmware-pico-250m --system-clock-khz 250000`
- `./tools/linux/load.sh --board pico --firmware-build-dir build/firmware-pico-250m --skip-build`
- `./.venv/bin/python tools/linux/spi_trace_benchmark.py --board pico --firmware-build-dir build/firmware-pico-250m --capture mosi --speed-hz 17500000 18000000 --trials 3`
- `./.venv/bin/python tools/linux/spi_trace_benchmark.py --board pico --firmware-build-dir build/firmware-pico-250m --capture mosi-miso --speed-hz 5800000 5900000 --trials 3`
- `./.venv/bin/python tools/linux/i2c_trace_test.py --channel 0 --bus 1 --sample-hz 4000000 --expect-transactions 112`

### Results

- hosted tests: `usb_app_test` passed with the new timeout-policy and saturation-guard coverage updates.
- MOSI: `17.5 MHz` passed `3/3`, and `18.0 MHz` also passed `3/3` with `sink=0 sampler=0 ring=0`.
- MOSI+MISO: `5.8 MHz` passed `3/3` with `sink=0 sampler=0 ring=0`; `5.9 MHz` improved to `2/3`, and the single failing trial reported downstream pressure with `sink=171 sampler=0 ring=0 peak=252`.
- I2C smoke: the standard `4.0 MHz` `i2cdetect -y 1` trace stayed healthy with `transactions=112 starts=112 stops=112 overruns=0 sticky=0`.
- counter notes: the early saturation guard no longer inflates sink-overrun counts per dropped continuation attempt; failing SPI trials now report bounded overflow counts instead of per-word spikes.

### Interpretation

- Splitting the MOSI and MOSI+MISO inner append kernels removed hot-loop branching without reopening the duplicated higher-level append control path.
- Requiring one idle stale poll before timeout close reduced premature timeout-driven fragmentation, and the focused SPI rerun improved on this bench rather than regressing.
- The early saturation guard now behaves as intended: it avoids wasting producer work when the ring is near full while still keeping overflow accounting honest.
- The SPI-focused firmware changes did not disturb the baseline I2C smoke path.

### Baseline Impact

- `docs/testlog/rp2040-benchmark-baseline.md` updated: no
- reason: the measured SPI edge improved versus the current published baseline, but the workflow says to leave the stable baseline unchanged unless explicitly requested.

## 2026-07-21 - SPI And I2C Benchmark Rerun

### Scope

- board: Raspberry Pi Pico (`RP2040`)
- firmware clock: `250 MHz`
- firmware/build: `build/firmware-pico-250m`
- reason: rerun the standard SPI benchmark sweeps and I2C trace bracket on the current firmware image

### Commands

- `./tools/linux/load.sh --board pico --firmware-build-dir build/firmware-pico-250m --skip-build`
- `./.venv/bin/python tools/linux/spi_trace_benchmark.py --board pico --firmware-build-dir build/firmware-pico-250m --capture mosi --speed-hz 15500000 16000000 16500000 17000000 17500000 18000000 --trials 3`
- `./.venv/bin/python tools/linux/spi_trace_benchmark.py --board pico --firmware-build-dir build/firmware-pico-250m --capture mosi-miso --speed-hz 5600000 5700000 5800000 5900000 --trials 3`
- `for hz in 4000000 10000000 11000000 12000000; do ./.venv/bin/python tools/linux/i2c_trace_test.py --channel 0 --bus 1 --sample-hz "$hz" --expect-transactions 112; done`

### Results

- MOSI: `15.5/16.0/16.5/17.0/17.5 MHz` all passed `3/3`; `18.0 MHz` failed `0/3` with downstream-limited loss (`sink>0 sampler=0 ring>0 peak=256`).
- MOSI+MISO: `5.6 MHz` passed `1/3`, `5.7 MHz` passed `1/3`, `5.8 MHz` passed `2/3`, and `5.9 MHz` passed `1/3`; every failing trial was downstream-limited (`sink>0 sampler=0 ring>0 peak=256`).
- I2C smoke: `4.0 MHz` stayed healthy with `transactions=112 starts=112 stops=112 overruns=0 sticky=0`.
- I2C sample-hz notes: `10.0 MHz`, `11.0 MHz`, and `12.0 MHz` all stayed healthy on the standard `i2cdetect -y 1` workload with `transactions=112 starts=112 stops=112 overruns=0 sticky=0`.

### Interpretation

- MOSI-only SPI matches the current published `250 MHz` baseline: `17.5 MHz` remains the highest clean point and `18.0 MHz` remains the first unstable point.
- MOSI+MISO SPI is materially worse than the current published baseline on this bench, because the expected clean range through `5.8 MHz` did not hold in this rerun.
- I2C is stronger than the current published `250 MHz` baseline on the standard smoke workload, because the prior unstable bracket at `11-12 MHz` stayed healthy in this rerun.
- The SPI failures remained downstream-limited rather than sampler-limited.

### Baseline Impact

- `docs/testlog/rp2040-benchmark-baseline.md` updated: no
- reason: this rerun produced mixed movement versus the current baseline, and the workflow says to leave the baseline unchanged unless explicitly requested.

## 2026-07-21 - SPI Simplification Follow-Up

### Scope

- board: Raspberry Pi Pico (`RP2040`)
- firmware clock: `250 MHz`
- firmware/build: `build/firmware-pico-250m`
- reason: validate the SPI append-path, boundary-state, and flush-policy simplification work on real hardware and confirm the standard I2C smoke path still stays healthy

### Commands

- `cmake --build build/tests --target usb_app_test && ./build/tests/usb_app_test`
- `./tools/linux/build.sh --board pico --firmware-build-dir build/firmware-pico-250m --system-clock-khz 250000`
- `./tools/linux/load.sh --board pico --firmware-build-dir build/firmware-pico-250m --skip-build`
- `./.venv/bin/python tools/linux/spi_trace_benchmark.py --board pico --firmware-build-dir build/firmware-pico-250m --capture mosi --speed-hz 17500000 18000000 --trials 3`
- `./.venv/bin/python tools/linux/spi_trace_benchmark.py --board pico --firmware-build-dir build/firmware-pico-250m --capture mosi-miso --speed-hz 5800000 5900000 --trials 3`
- `./.venv/bin/python tools/linux/i2c_trace_test.py --channel 0 --bus 1 --sample-hz 4000000 --expect-transactions 112`

### Results

- hosted tests: `usb_app_test` passed after updating the overflow regression for the new continuation-fragment policy.
- MOSI: `17.5 MHz` fell to `2/3`; the failing trial was downstream-limited with `sink=2 sampler=0 ring=2 peak=256`. `18.0 MHz` remained unstable at `0/3` with heavier downstream-limited loss.
- MOSI+MISO: `5.8 MHz` fell to `1/3`; both failing trials were downstream-limited with `sink>0 sampler=0 ring>0 peak=256`. `5.9 MHz` remained unstable at `0/3`.
- I2C smoke: the standard `4.0 MHz` `i2cdetect -y 1` trace stayed healthy with `transactions=112 starts=112 stops=112 overruns=0 sticky=0`.
- I2C sample-hz notes: this follow-up only reran the conservative smoke point because the code change was isolated to the SPI path.

### Interpretation

- The SPI producer-side simplification is functionally correct in hosted coverage, but this bench rerun showed a measurable throughput regression at the previous `250 MHz` clean SPI edges.
- The failure signature stayed downstream-limited (`sink`/`ring`, `peak=256`) rather than sampler-limited, so the regression appears to be in packetization or flush behavior rather than raw sample extraction.
- The standard I2C smoke path was unaffected by the SPI-only firmware change.

### Baseline Impact

- `docs/testlog/rp2040-benchmark-baseline.md` updated: no
- reason: the run indicates a regression that should be investigated first rather than published as a new stable baseline.

## 2026-07-21 - I2C Simplification Sweep

### Scope

- board: Raspberry Pi Pico (`RP2040`)
- firmware clock: `150 MHz`, `200 MHz`, `225 MHz`, `250 MHz`
- firmware/build: `build/firmware-pico-150m`, `build/firmware-pico-200m`, `build/firmware-pico-225m`, `build/firmware-pico-250m`
- reason: validate the I2C monitor simplification work, remeasure the RP2040 I2C smoke-workload ceiling, and run the required SPI regression check

### Commands

- `cmake --build build/tests --target usb_app_test i2c_monitor_runtime_test && ./build/tests/usb_app_test && ./build/tests/i2c_monitor_runtime_test`
- `for spec in 150000:build/firmware-pico-150m 200000:build/firmware-pico-200m 225000:build/firmware-pico-225m 250000:build/firmware-pico-250m; do ./tools/linux/build.sh --board pico --firmware-build-dir ... --system-clock-khz ...; done`
- `for spec in 150000:build/firmware-pico-150m:'3750000 4000000 4250000 5000000 5500000 6000000 6500000 7000000 8000000 9000000' 200000:build/firmware-pico-200m:'5250000 5500000 5750000 6000000 6500000 7000000 7500000 8000000 8500000 10000000 11000000' 225000:build/firmware-pico-225m:'5750000 6000000 6250000 6500000 7000000 7500000 8000000 9000000 9500000 10000000 11000000' 250000:build/firmware-pico-250m:'6600000 6650000 6750000 7500000 8000000 8500000 9000000 10000000 11000000 12000000 15000000 18000000 20000000'; do ./tools/linux/load.sh --board pico --firmware-build-dir ... --skip-build; ./.venv/bin/python tools/linux/i2c_trace_test.py --channel 0 --bus 1 --sample-hz ... --expect-transactions 112; done`
- `./tools/linux/load.sh --board pico --firmware-build-dir build/firmware-pico-250m --skip-build`
- `./.venv/bin/python tools/linux/spi_trace_benchmark.py --board pico --firmware-build-dir build/firmware-pico-250m --capture mosi --speed-hz 18500000 19000000 --trials 3`
- `./.venv/bin/python tools/linux/spi_trace_benchmark.py --board pico --firmware-build-dir build/firmware-pico-250m --capture mosi --speed-hz 17500000 18000000 --trials 3`
- `./.venv/bin/python tools/linux/spi_trace_benchmark.py --board pico --firmware-build-dir build/firmware-pico-250m --capture mosi-miso --speed-hz 5700000 5800000 5900000 --trials 3`

### Results

- hosted tests: `usb_app_test` and `i2c_monitor_runtime_test` passed after the decode-path and overflow-policy changes.
- MOSI: `17.5 MHz` passed `3/3`; `18.0 MHz`, `18.5 MHz`, and `19.0 MHz` all failed with downstream-limited `sink=ring peak=256` behavior.
- MOSI+MISO: `5.7 MHz` and `5.8 MHz` passed `3/3`; `5.9 MHz` fell to `1/3`, matching the earlier unstable edge.
- I2C smoke: every old cross-clock unstable point used in the previous baseline passed cleanly after the simplification work.
- I2C sample-hz notes: `150 MHz -> 6.0 MHz` clean and `6.5 MHz` unstable; `200 MHz -> 7.5 MHz` clean and `8.0 MHz` unstable; `225 MHz -> 9.0 MHz` clean and `9.5 MHz` unstable; `250 MHz -> 10.0 MHz` clean and `11.0 MHz` unstable. On `250 MHz`, `12.0 MHz` already dropped to `90/112` with `overruns=22 sticky=1` and `15.0 MHz+` degraded much further.

### Interpretation

- The direct decode-to-builder path plus idle-word skipping materially increased the standard `i2cdetect -y 1` smoke-workload ceiling on every measured RP2040 clock point.
- The new overflow policy now preserves `running` state and recovers in place, but `sticky=1` still correctly reports that the channel crossed an overrun boundary on failing runs.
- The standard `i2cdetect` workload is now sparse enough that the new ceiling is much higher; the clean point at `250 MHz` moved from `6.6 MHz` to `10.0 MHz`.
- SPI MOSI+MISO stayed aligned with the prior baseline, but MOSI-only throughput regressed on this bench from `18.5 MHz` clean to `17.5 MHz` clean, with the new failures limited by downstream ring saturation rather than sampler loss.

### Baseline Impact

- `docs/testlog/rp2040-benchmark-baseline.md` updated: yes
- reason: the RP2040 I2C cross-clock ceiling materially improved and the current 250 MHz MOSI-only SPI edge no longer matches the previous baseline on this bench.

## 2026-07-21 - I2C Buffer And Plateau Follow-Up

### Scope

- board: Raspberry Pi Pico (`RP2040`)
- firmware clock: `250 MHz`
- firmware/build: `build/firmware-pico-250m`
- reason: validate the `128`-word raw buffers, stable-word decoder skipping, and active-channel iteration tightening

### Commands

- `cmake --build build/tests --target usb_app_test i2c_monitor_runtime_test && ./build/tests/usb_app_test && ./build/tests/i2c_monitor_runtime_test`
- `./tools/linux/build.sh --board pico --firmware-build-dir build/firmware-pico-250m --system-clock-khz 250000`
- `./tools/linux/load.sh --board pico --firmware-build-dir build/firmware-pico-250m --skip-build`
- `./.venv/bin/python tools/linux/i2c_trace_test.py --channel 0 --bus 1 --sample-hz 10000000 --expect-transactions 112`
- `./.venv/bin/python tools/linux/i2c_trace_test.py --channel 0 --bus 1 --sample-hz 11000000 --expect-transactions 112`
- `./.venv/bin/python tools/linux/i2c_trace_test.py --channel 0 --bus 1 --sample-hz 12000000 --expect-transactions 112`
- `./.venv/bin/python tools/linux/i2c_trace_test.py --channel 0 --bus 1 --sample-hz 14000000 --expect-transactions 112`
- `./.venv/bin/python tools/linux/i2c_trace_test.py --channel 0 --bus 1 --sample-hz 16000000 --expect-transactions 112`
- `./.venv/bin/python tools/linux/i2c_trace_test.py --channel 0 --bus 1 --sample-hz 18000000 --expect-transactions 112`
- `./.venv/bin/python tools/linux/i2c_trace_test.py --channel 0 --bus 1 --sample-hz 20000000 --expect-transactions 112`
- `./.venv/bin/python tools/linux/spi_trace_benchmark.py --board pico --firmware-build-dir build/firmware-pico-250m --capture mosi --speed-hz 17500000 18000000 --trials 3`
- `./.venv/bin/python tools/linux/spi_trace_benchmark.py --board pico --firmware-build-dir build/firmware-pico-250m --capture mosi-miso --speed-hz 5800000 5900000 --trials 3`

### Results

- hosted tests: `usb_app_test` and `i2c_monitor_runtime_test` passed with the new plateau regression coverage.
- I2C smoke: `10/11/12/14/16/18/20 MHz` all stayed healthy on the standard `i2cdetect -y 1` workload with `transactions=112 starts=112 stops=112 overruns=0 sticky=0`.
- I2C sample-hz notes: the standard smoke workload no longer exposed a failure point in this focused `250 MHz` rerun; `20 MHz` was still clean.
- MOSI: `17.5 MHz` passed `3/3`; `18.0 MHz` improved slightly to `1/3` but still failed overall with downstream-limited `sink=ring peak=256` on the failing trials.
- MOSI+MISO: `5.8 MHz` regressed to `1/3`; `5.9 MHz` remained `1/3`, with failures again downstream-limited by `sink=ring peak=256`.

### Interpretation

- The larger raw buffers plus stable-word skipping materially reduced I2C producer pressure on the standard smoke workload.
- On this workload, the I2C ceiling is no longer bracketed by the previous `10-12 MHz` region; a heavier workload is now needed if the goal is to measure a meaningful new RP2040 I2C edge.
- The SPI spot check did not show a clear improvement and remained dominated by downstream ring saturation rather than sampler loss.

### Baseline Impact

- `docs/testlog/rp2040-benchmark-baseline.md` updated: no
- reason: this follow-up reran only the `250 MHz` image, and the standard I2C smoke workload no longer brackets a stable ceiling; a fresh multi-clock or heavier-workload sweep should decide the next stable baseline update.

## 2026-07-21 - Combined-Burst Stress Follow-Up

### Scope

- board: Raspberry Pi Pico (`RP2040`)
- firmware clock: `250 MHz`
- firmware/build: `build/firmware-pico-250m`
- reason: use the existing repeated-start combined-burst workload to find a more meaningful I2C ceiling after the buffer and decoder optimizations

### Commands

- `./.venv/bin/python tools/linux/i2c_trace_test.py --channel 0 --bus 1 --sample-hz 10000000 --workload combined-burst --target-address 0x50 --read-length 4 --repeat-count 64 --expect-transactions 0`
- `./.venv/bin/python tools/linux/i2c_trace_test.py --channel 0 --bus 1 --sample-hz 20000000 --workload combined-burst --target-address 0x50 --read-length 4 --repeat-count 64 --expect-transactions 0`
- `./.venv/bin/python tools/linux/i2c_trace_test.py --channel 0 --bus 1 --sample-hz 20000000 --workload combined-burst --target-address 0x50 --read-length 4 --repeat-count 256 --expect-transactions 0`
- `./.venv/bin/python tools/linux/i2c_trace_test.py --channel 0 --bus 1 --sample-hz 20000000 --workload combined-burst --target-address 0x50 --read-length 4 --repeat-count 1024 --expect-transactions 0`
- `for hz in 30000000 40000000 50000000; do ./.venv/bin/python tools/linux/i2c_trace_test.py --channel 0 --bus 1 --sample-hz "$hz" --workload combined-burst --target-address 0x50 --read-length 4 --repeat-count 1024 --expect-transactions 0; done`

### Results

- repeated-start shape: every run preserved the expected `starts = 2 * stops` shape.
- traffic outcome: all `i2ctransfer` operations NACKed on address `0x50`, so the Linux-side command reported `successes=0 failures=...`, but the bus activity was still traced correctly.
- I2C stress status: `10/20/30/40/50 MHz` all stayed healthy on the `1024`-transfer combined-burst workload with `overruns=0 sticky=0`.
- packet volume: the `1024`-transfer runs produced `1025` packets, `1024` transactions, `2048` starts, `1024` stops, `2048` data events, and `2048` NACK events.

### Interpretation

- The existing combined-burst workload is a better repeated-start shape check than `i2cdetect`, but on this bench it still does not bracket the post-optimization I2C ceiling.
- Because address `0x50` does not ACK, each transfer aborts early and does not create a byte-heavy stream; this limits how much sustained decode pressure the workload can generate even at high `sample-hz`.
- A meaningful next ceiling test now needs either a real ACKing I2C target or a different workload that keeps the bus active for longer per transaction.

### Baseline Impact

- `docs/testlog/rp2040-benchmark-baseline.md` updated: no
- reason: the repeated-start stress workload still did not expose a failure point on this bench, so there is not yet a stable new ceiling to publish.

## 2026-07-21 - USB Bulk API Removal Benchmark Rerun

### Scope

- board: Raspberry Pi Pico (`RP2040`)
- firmware clock: `250 MHz`
- firmware/build: `build/firmware-pico-250m`
- reason: validate SPI and I2C trace behavior after removing unused USB bulk helper APIs and moving vendor flush ownership to the outer service loop

### Commands

- `cmake --build build/tests --target usb_app_test && ./build/tests/usb_app_test`
- `./tools/linux/build.sh --board pico --firmware-build-dir build/firmware-pico-250m --system-clock-khz 250000`
- `./tools/linux/load.sh --board pico --firmware-build-dir build/firmware-pico-250m --skip-build`
- `./.venv/bin/python tools/linux/spi_trace_benchmark.py --board pico --firmware-build-dir build/firmware-pico-250m --capture mosi --speed-hz 15500000 16000000 16500000 17000000 17500000 18000000 --trials 3`
- `./.venv/bin/python tools/linux/spi_trace_benchmark.py --board pico --firmware-build-dir build/firmware-pico-250m --capture mosi-miso --speed-hz 5600000 5700000 5800000 5900000 --trials 3`
- `./.venv/bin/python tools/linux/i2c_trace_test.py --channel 0 --bus 1 --sample-hz 4000000 --expect-transactions 112`

### Results

- hosted tests: `usb_app_test` currently fails to compile because it still references removed APIs (`usb_bulk_write`, `usb_bulk_stream_write`); benchmark and hardware runs were still executed on flashed firmware.
- MOSI: `15.5/16.0/16.5/17.0/17.5 MHz` passed `3/3`; `18.0 MHz` failed `0/3`. Failing trials remained downstream-limited with `sink>0 sampler=0 ring=0 peak=255`.
- MOSI+MISO: `5.6 MHz` passed `3/3`; `5.7 MHz` passed `2/3`; `5.8 MHz` passed `1/3`; `5.9 MHz` passed `0/3`. Failing trials remained downstream-limited with `sink>0 sampler=0 ring=0 peak=255`.
- I2C smoke: standard `4.0 MHz` `i2cdetect -y 1` trace stayed healthy with `transactions=112 starts=112 stops=112 overruns=0 sticky=0`.
- I2C sample-hz notes: this run executed the conservative smoke point only.

### Interpretation

- SPI MOSI stayed aligned with the current baseline envelope (`17.5 MHz` clean, `18.0 MHz` unstable).
- SPI MOSI+MISO regressed versus the current baseline in this rerun (`5.8 MHz` and `5.9 MHz` were unstable).
- Failure signatures remained downstream-limited rather than sampler-limited, consistent with USB/ring drain pressure.
- I2C smoke behavior remained healthy and unchanged.

### Baseline Impact

- `docs/testlog/rp2040-benchmark-baseline.md` updated: no
- reason: results are mixed versus the current baseline and include a hosted-test compile gap that should be resolved first.

## 2026-07-21 - Post-Test-Fix SPI And I2C Rerun

### Scope

- board: Raspberry Pi Pico (`RP2040`)
- firmware clock: `250 MHz`
- firmware/build: `build/firmware-pico-250m`
- reason: rerun standard SPI and I2C hardware benchmarks after repairing hosted test coverage for removed USB bulk APIs

### Commands

- `./tools/linux/build.sh --board pico --firmware-build-dir build/firmware-pico-250m --system-clock-khz 250000`
- `./tools/linux/load.sh --board pico --firmware-build-dir build/firmware-pico-250m --skip-build`
- `./.venv/bin/python tools/linux/spi_trace_benchmark.py --board pico --firmware-build-dir build/firmware-pico-250m --capture mosi --speed-hz 15500000 16000000 16500000 17000000 17500000 18000000 --trials 3`
- `./.venv/bin/python tools/linux/spi_trace_benchmark.py --board pico --firmware-build-dir build/firmware-pico-250m --capture mosi-miso --speed-hz 5600000 5700000 5800000 5900000 --trials 3`
- `./.venv/bin/python tools/linux/i2c_trace_test.py --channel 0 --bus 1 --sample-hz 4000000 --expect-transactions 112`

### Results

- MOSI: `15.5/16.0/16.5/17.0/17.5 MHz` passed `3/3`; `18.0 MHz` failed `0/3`.
- MOSI failure signature: downstream-limited with `sink>0 sampler=0 ring=0 peak=255`.
- MOSI+MISO: `5.6 MHz` passed `3/3`; `5.7 MHz` passed `1/3`; `5.8 MHz` passed `2/3`; `5.9 MHz` passed `0/3`.
- MOSI+MISO failure signature: downstream-limited with `sink>0 sampler=0 ring=0 peak=255`.
- I2C smoke: `transactions=112 starts=112 stops=112 overruns=0 sticky=0` at `4.0 MHz`.
- device status: `running hz=4000000` with no monitor overrun/sticky errors.

### Interpretation

- MOSI remains aligned with the current `250 MHz` baseline envelope: `17.5 MHz` clean and `18.0 MHz` unstable.
- MOSI+MISO remains noisier than the current baseline in this rerun; `5.8 MHz` did not stay `3/3` and `5.9 MHz` stayed unstable.
- Failure modes remained downstream-limited rather than sampler-limited.
- I2C smoke remained healthy and unchanged.

### Baseline Impact

- `docs/testlog/rp2040-benchmark-baseline.md` updated: no
- reason: SPI movement is mixed versus current baseline and does not justify a baseline change.
