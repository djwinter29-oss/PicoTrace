# .NET Host Library

This folder contains the PicoTrace .NET host implementation in `PicoTrace/` and focused logic tests in `PicoTrace.Tests/`.

Implementation-level design notes for this package live under [docs/details/](docs/details/README.md).

The host now mirrors the Python package split:

- `Trace/`: fixed packet models, decode helpers, channel filtering, and USB bulk trace transport
- `Control/`: HID command framing, response decoding, and the `HidControlClient`
- `App/`: CLI-oriented control operations and trace streaming helpers

It includes a small host-side trace library for the vendor bulk trace stream:

- incremental trace-packet framing from bulk IN bytes
- shared packet-header decoding
- I2C event decoding
- SPI MOSI / MOSI+MISO payload decoding

It also includes a HID control library for the device control channel:

- fixed 64-byte PicoTrace HID command/response framing
- stream enable and device status control
- I2C monitor control and status queries
- SPI monitor control and status queries

It also includes a small CLI app for control-first operation:

- configure monitoring from the terminal
- use the same control and trace library surfaces for both subcommands and interactive mode
- fall back to foreground streaming in the current console
- continuously decode and print matching channel traffic

It targets the current firmware USB surfaces:

- VID: `0xCAFE`
- PID: `0x4003`
- bulk IN endpoint: `0x83`
- HID control IN endpoint: `0x84`

The current `.NET` CLI does not implement the Python host's Windows detached monitor window manager. Monitoring runs in the current console for both subcommand and interactive workflows.

SPI monitor control remains bus-owned, matching the firmware contract. Configuring one logical SPI channel applies a shared bus configuration on that channel's owning bus, and stopping one logical SPI channel removes only that channel from the shared bus mask, disabling the bus only when no sibling channels remain selected.

Monitor shutdown is intentionally best-effort. The host always tries to send the matching I2C or SPI stop command, but it does not treat a stop failure as fatal during foreground stream exit or final application shutdown.

Channel selection is intentionally lightweight on the host side. The trace path uses the requested channel as a filter over decoded packets, so a channel with no matching traffic simply prints nothing.

## Prerequisites

- .NET 8 SDK or newer
- a libusb-compatible backend for the PicoTrace USB interfaces

Platform notes:

- Windows: bind the vendor bulk interface to WinUSB or libusb with Zadig
- Linux: install libusb and ensure your user has permission to access the device

## CLI

Run the CLI with:

```bash
dotnet run --project host/dotnet/PicoTrace -- --help
```

Supported commands:

- `status`
- `stream on`
- `stream off`
- `led on`
- `led off`
- `reboot [--yes]`
- `i2c --channel <0-3> --sample-hz <hz> [--no-stream]`
- `spi --channel <0-5> [--capture MOSI|MOSI_MISO] [--spi-mode 0-3] [--timeout-us <us>] [--no-stream]`
- `trace --channel <0-255>`

Examples:

```bash
dotnet run --project host/dotnet/PicoTrace -- status
dotnet run --project host/dotnet/PicoTrace -- i2c --channel 0 --sample-hz 1000000
dotnet run --project host/dotnet/PicoTrace -- spi --channel 4 --capture MOSI_MISO --spi-mode 0 --timeout-us 100
dotnet run --project host/dotnet/PicoTrace -- trace --channel 0
```

If no command is provided, the app starts a simple interactive console menu.

Build or debug the solution from Visual Studio with:

```text
host/dotnet/PicoTrace.sln
```

## Library Notes

The trace decoder keeps the fixed PicoTrace packet contract explicit:

- packet size: `128` bytes maximum
- header size: `16` bytes
- trace types: `I2C`, `SPI`

The HID control layer keeps the fixed report contract explicit:

- report size: `64` bytes
- stream enable/disable
- I2C per-channel configure/status
- SPI per-bus configure/status
- LED and reboot control

Like the Python host, monitor stop/disable cleanup is best-effort. The CLI tries to stop the configured monitor on exit or startup failure, but cleanup failures do not replace the original error.

## Tests

Run the focused logic tests with:

```bash
dotnet test host/dotnet/PicoTrace.Tests/PicoTrace.Tests.csproj
```

The current automated tests focus on pure packet decode, HID protocol packing/validation, SPI bus-mask control helpers, and CLI numeric argument validation. Live-device USB and HID interaction is not yet covered by automated tests.
