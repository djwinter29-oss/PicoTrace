# AGENTS

## Cursor Cloud specific instructions

PicoTrace is RP2040/RP2350 firmware (C, CMake + Pico SDK) plus host libraries in
Python (`host/python`) and .NET (`host/dotnet`), and native host tests for the
firmware (`firmware/tests`). Standard build/run commands live in `README.md`,
`.github/workflows/pr-tests.yml`, and `tools/linux/`; the notes below only cover
non-obvious, environment-specific caveats.

### Components and how to test/build/run them

| Component | Command (from repo root) |
|-----------|--------------------------|
| Firmware host tests (C) | `./tools/linux/test.sh --skip-firmware-build` (configures `firmware/tests`, builds, runs `ctest`) |
| Python host lib + tests | `cd host/python && /workspace/.venv/bin/python -m pytest -q` |
| Python CLI (app) | `/workspace/.venv/bin/picotrace --help` |
| .NET host lib + tests | `cd host/dotnet && dotnet test PicoTrace.Tests/PicoTrace.Tests.csproj --nologo` |
| .NET CLI (app) | `cd host/dotnet && dotnet run --project PicoTrace/PicoTrace.csproj -- --help` |
| Firmware image (pico) | `./tools/linux/build.sh --board pico` (add `--board pico2` for RP2350); outputs `build/firmware-pico/picotrace.uf2` |

There is no separate linter configured; CI (`pr-tests.yml`) only builds and runs
the three test suites above.

### Non-obvious caveats

- Host compiler must be GCC, not Clang. The default `cc`/`c++` alternatives point
  to `clang`, and the firmware's host-side `picotool` build fails with
  `cannot find -lstdc++` under clang. Setup already switched the defaults to gcc
  (`update-alternatives --set cc /usr/bin/gcc`, `... c++ /usr/bin/g++`), persisted
  in the VM snapshot. If a fresh environment reverts this, re-run those two
  `update-alternatives --set` commands before building firmware.
- Pico SDK lives at `/workspace/.pico-sdk` (shallow clone with submodules).
  `tools/linux/.env.sh` exports `PICO_SDK_PATH` and is auto-sourced by
  `build.sh`/`test.sh`/`load.sh`, so firmware builds need no manual env export.
- The Python virtualenv is at repo root `/workspace/.venv` (not under
  `host/python`). Use `/workspace/.venv/bin/python`. `host/python/requirements.txt`
  uses `-e .`, so install with `pip install -e host/python` (path form) rather than
  running it from an arbitrary directory.
- No USB hardware is attached in the cloud VM. The CLI control commands
  (`status`, `stream`, `led`, `reboot`, `i2c`, `spi`, `trace`) and
  `tools/linux/load.sh` / OpenOCD / the `spi_trace_benchmark.py` need a real
  PicoTrace device (and Debug Probe) and will fail here. To exercise core
  functionality hardware-free, drive the transport-agnostic trace-decode path in
  the host libraries (`picotrace.trace.decode` / `PicoTrace.Trace.TraceDecoder`),
  which is exactly what the host test suites cover.
