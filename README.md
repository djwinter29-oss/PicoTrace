# PicoTrace

PicoTrace is a low-cost protocol tracing tool built around the RP2040 and RP2350 families,
currently centered on passive SPI transaction capture and passive I2C transaction capture.

The firmware uses a shared USB device model across both protocols: TinyUSB composite descriptors,
a bounded CDC command path, a HID control path, and a vendor bulk stream for high-rate trace
delivery. The host-visible control model stays the same whether the producer side is packaging SPI
traffic or I2C traffic.

For reliable streaming, the preferred shape is to keep all USB stack interaction on one core and
move sampling or packet preparation to the other core only when needed. USB class traffic still
shares the same device stack and bus, so splitting CDC, HID, and vendor transfers across cores is
usually less reliable than keeping USB ownership on one core and handing off data through a buffer.

## What PicoTrace provides

- RP2040 and RP2350 firmware project structure using CMake and the Pico SDK
- TinyUSB composite USB device configuration for trace streaming and low-rate host control
- USB descriptor definitions and serial-number wiring
- A CDC interface for simple device CLI access and status exchange
- A HID interface for structured host control and status exchange
- A vendor bulk interface for trace data streaming
- A fixed-slot trace packet ring sized for SPI and I2C transaction capture workloads
- Host-side capture helpers for bulk trace intake
- Build, test, and programming scripts for Windows and Linux

## Current protocol focus

- Passive SPI transaction capture
- Passive I2C transaction capture
- A shared host-side control and streaming model across both protocols

## Repository layout

- `firmware/`: RP2040 and RP2350 firmware sources, TinyUSB configuration, and tests
- `firmware/src/config/tusb_config.h`: TinyUSB compile-time USB configuration
- `firmware/src/driver/led.*`: minimal board LED driver
- `firmware/src/driver/system.*`: minimal reboot/system wrapper for board-level reset actions
- `firmware/src/usb/usb_cdc.*`: CDC transport and raw byte queue
- `firmware/src/usb/usb_bulk.*`: Bulk streaming transport code
- `firmware/src/usb/usb_hid.*`: HID transport and request/response state code
- `firmware/src/usb_stream.*`: trace stream plumbing that bridges completed trace packets from `trace_ring` into the vendor bulk transport
- `firmware/src/usb/usb_descriptors.c`: USB device, interface, and string descriptor definitions
- `tools/`: helper scripts for build, flash, and host-side CDC testing
- `docs/`: short project notes covering trace transport and capture design
- `docs/README.md`: documentation index covering architecture, detail design, and hardware/setup notes

## Architecture documents

- `docs/architecture/README.md`: entry point for architecture-level design notes
- `docs/architecture/firmware-architecture.md`: top-level firmware architecture, core split, and component interaction diagrams
- `docs/architecture/interface-and-synchronization.md`: interface boundaries, synchronization mechanisms, and race-avoidance strategy
- `docs/details/`: component-level detail design notes that support the architecture view

## Streaming design

See `docs/streaming-design.md` for the recommended ownership model for passive SPI and I2C capture
with reliable streaming plus low-rate host control.

## Building for Pico and Pico 2

Firmware builds require the Raspberry Pi Pico SDK.

- Windows: set `PICO_SDK_PATH` in your shell or in `tools/windows/.env.ps1`, or place the SDK at `C:\src\pico-sdk` to use the script's default lookup.
- Linux: export `PICO_SDK_PATH` before running the firmware build or load scripts.

Programming through the load scripts also requires OpenOCD. The Linux load script expects `openocd` on `PATH` by default, and both Windows and Linux let you override the target config when needed.

The firmware CMake project defaults to `PICO_BOARD=pico`.

Default Pico build and load commands:

```powershell
.\tools\windows\build.ps1
.\tools\windows\load.ps1
```

```bash
./tools/linux/build.sh
./tools/linux/load.sh
```

If you want to pass the board explicitly for Raspberry Pi Pico:

```powershell
.\tools\windows\build.ps1 -Board pico
```

PowerShell parameter names are case-insensitive, so this works too:

```powershell
.\tools\windows\build.ps1 -board pico
```

```bash
./tools/linux/build.sh -Board pico
```

Linux also accepts the long option form:

```bash
./tools/linux/build.sh --board pico
```

To build for Raspberry Pi Pico 2:

```powershell
.\tools\windows\build.ps1 -Board pico2
```

or:

```powershell
.\tools\windows\build.ps1 -board pico2
```

```bash
./tools/linux/build.sh -Board pico2
```

or:

```bash
./tools/linux/build.sh --board pico2
```

The load scripts choose the default OpenOCD target from the board name:

- `pico*` uses `target/rp2040.cfg`
- `pico2*` uses `target/rp2350.cfg`

So Pico 2 normally only needs the board:

```powershell
.\tools\windows\load.ps1 -Board pico2
```

or:

```powershell
.\tools\windows\load.ps1 -board pico2
```

```bash
./tools/linux/load.sh -Board pico2
```

or:

```bash
./tools/linux/load.sh --board pico2
```

If your local OpenOCD package uses a different target filename, you can still override it:

```powershell
.\tools\windows\load.ps1 -Board pico2 -OpenOcdTarget target/rp2350.cfg
```

```bash
./tools/linux/load.sh -Board pico2 --openocd-target target/rp2350.cfg
```

The older positional Linux forms still work if you need them:

```bash
./tools/linux/build.sh "" build/tests pico2
OPENOCD_TARGET=target/rp2350.cfg ./tools/linux/load.sh "" "" pico2
```

```bash
./tools/linux/load.sh --Board pico2 --openocd-target target/rp2350.cfg
```

If you configure CMake directly, pass the board explicitly when needed:

```powershell
cmake -S firmware -B build/firmware-pico2 -DPICO_BOARD=pico2
cmake --build build/firmware-pico2
```

## Scope

The current scope is a practical RP2040/RP2350-based protocol tracer with shared USB transport and
host control patterns across SPI and I2C capture. Product-specific protocol decoding, richer
host-side analysis, and hardware front-end expansion can evolve on top of that core without
changing the basic USB ownership model.