# App Design

## Purpose

The `PicoTrace.App` namespace provides a pure CLI operator workflow on top of the existing control and trace libraries.

The current app is intentionally control-first:

- configure monitoring from the terminal
- use the same control helpers for interactive and subcommand flows
- stream matching channel traffic in the current console
- keep waiting when no packets are currently available

Unlike the Python host, the current .NET host does not implement detached child monitor windows. Streaming stays in the current console on all supported platforms.

## Current Module Layout

- `Program.cs`: CLI entry point, argument parsing, and interactive orchestration
- `App/ControlOperations.cs`: short-lived HID control helpers used by the CLI
- `App/TraceStreaming.cs`: trace formatting and one-channel stream loop helpers

## CLI Responsibilities

The CLI owns the operator-facing control flow.

Current responsibilities:

- read shared device status over HID
- enable or disable streaming
- control the board LED and reboot action
- configure one I2C channel sample rate
- configure one SPI logical channel on its owning bus
- stream one logical trace channel to stdout
- stream all decoded trace traffic to stdout without applying a host-side channel filter

The CLI intentionally uses the existing `PicoTrace.Control` and `PicoTrace.Trace` APIs instead of talking to USB directly.

## Interaction Model

The current CLI supports two operator styles:

- one-shot subcommands such as `status`, `i2c`, `spi`, and `trace`
- an interactive terminal menu when `dotnet run --project host/dotnet/PicoTrace` is launched with no subcommand

Both styles are backed by the same control and trace library surfaces.

Cleanup is intentionally best-effort. The app tries to send the matching stop command when a configured monitor closes, when foreground streaming exits, and when the interactive shell shuts down. If that stop command fails, the host still finishes local cleanup and exits.

Channel selection is intentionally lightweight at the CLI boundary. The trace stream path treats the selected channel as a host-side packet filter, so a selection with no matching traffic simply produces no output.

For workflows that already use another control path, the CLI also supports `trace --all`. That path skips channel registration entirely, assumes another interface such as the CDC CLI owns configuration, and keeps the bulk trace monitor open until the operator stops it.

## SPI Ownership Model

The CLI exposes logical SPI channels, but the firmware control surface is bus-owned.

Current behavior:

- configuring one logical SPI channel reads the current bus status, merges the selected logical-channel bit into the shared bus mask, and reapplies one bus configuration
- stopping one logical SPI channel reads the current bus status, clears only that logical-channel bit, and disables the bus only if no sibling channel bits remain

That keeps the .NET host aligned with the firmware contract and with the Python host's bus-owned SPI lifecycle semantics.

## Data Flow

Configuration flow:

1. the user selects a CLI action or subcommand
2. the app opens a short-lived `HidControlClient`
3. the app sends the corresponding HID control request
4. the app enables trace streaming if configuration succeeds
5. the app starts foreground streaming for the selected logical channel unless `--no-stream` was requested

Streaming flow:

1. the CLI creates a `TraceChannelRegistry` for one channel, or leaves it empty for `trace --all`
2. the CLI calls `UsbBulkTraceTransport.IterTracePackets(...)`
3. packets not matching the registered channel are filtered out by the trace library
4. matching packets are decoded into text lines and printed to stdout
5. closing a configured monitor attempts the matching I2C or SPI stop command before the host session exits
6. stop commands are cleanup-only and are treated as best-effort, so a stop failure does not block local process teardown or final app exit

## Current Limits

The current app is intentionally small.

- it is a terminal operator tool, not a full analyzer UI
- it prints decoded packets as text rather than rendering protocol-specific tables or waveforms
- channel filtering is still host-side filtering over the full trace stream
- monitor windows are not split into detached child consoles the way the Python Windows workflow is

The current automated test entry point is `dotnet test host/dotnet/PicoTrace.Tests/PicoTrace.Tests.csproj`.

If future work needs richer analysis or separate monitor consoles, that should be built on top of the existing control and trace library boundaries rather than folded into the protocol layer.