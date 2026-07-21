# RP2040 Benchmark Test Log Template

Copy this template into `rp2040-benchmark-testlog.md` when recording a new RP2040 SPI benchmark or I2C trace-validation run.

```md
## YYYY-MM-DD - Short Title

### Scope

- board:
- firmware clock:
- firmware/build:
- reason:

### Commands

- `...`
- `...`

### Results

- hosted tests:
- MOSI:
- MOSI+MISO:
- I2C smoke:
- I2C sample-hz notes:

### Interpretation

- ...
- ...
- If I2C was exercised above the conservative `4000000` smoke-test rate, record the highest clean `sample-hz`, the first unstable point, and whether the status stayed `running`.

### Baseline Impact

- `docs/testlog/rp2040-benchmark-baseline.md` updated: yes|no
- reason:
```
