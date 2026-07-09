# App Design

## Purpose

The `picotrace.app` package provides a pure CLI operator workflow on top of the existing control and trace libraries.

The current app is intentionally control-first:

- configure monitoring from the terminal
- keep the main control console available on Windows while monitors run in separate windows
- fall back to foreground streaming in the current console on platforms without detached monitor consoles
- continuously decode and print channel traffic
- keep waiting when no packets are currently available

## Current Module Layout

- `app/cli.py`: CLI entry point, monitor lifecycle, and interactive orchestration
- `app/control_ops.py`: short-lived HID control helpers used by the CLI
- `app/trace_stream.py`: trace formatting and one-channel stream loop helpers
- `app/__init__.py`: package surface exposing `main`

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

The CLI intentionally uses the existing `picotrace.control` API instead of talking to USB directly.

## Interaction Model

The current CLI supports two operator styles:

- one-shot subcommands such as `status`, `i2c`, `spi`, and `trace`
- an interactive terminal menu when `picotrace` is launched with no subcommand

Both styles are backed by the same control and trace library surfaces.

In interactive mode on Windows, channel monitors are managed as separate child console windows. Reconfiguring the same logical channel ends that channel's current monitor session first and then starts a new one with the updated configuration. On platforms without detached console support, interactive monitoring uses the current console instead.

Cleanup is intentionally best-effort. The app tries to send the matching stop command when a configured monitor closes, when foreground streaming exits, and when the interactive shell shuts down. If that stop command fails, the host still finishes local cleanup and exits. This keeps the operator workflow simple when the monitored session is already over or the device is no longer reachable.

Channel selection is intentionally lightweight at the CLI boundary. The trace stream path treats the selected channel as a host-side packet filter, so a selection with no matching traffic simply produces no output. The app does not add an extra strict host-only policy layer for that case.

For workflows that already use another control path, the CLI also supports `trace --all`. That path leaves the `TraceChannelRegistry` empty, assumes another interface such as the CDC CLI owns configuration, and keeps the bulk trace monitor open until the operator stops it.

## Data Flow

Configuration flow:

1. the user selects a CLI action or subcommand
2. the app opens a short-lived `HidControlClient`
3. the app sends the corresponding HID control request
4. the app enables trace streaming if configuration succeeds
5. the app starts a separate monitor console for the selected logical channel unless `--no-stream` or `--foreground` was requested
6. on platforms without detached console support, the app falls back to foreground streaming in the current console

Streaming flow:

1. the CLI creates a `TraceChannelRegistry` for one channel, or leaves it empty for `trace --all`
2. the CLI calls `iter_trace_packets(...)`
3. packets not matching the registered channel are filtered out by the trace library
4. matching packets are decoded into text lines and printed to stdout
5. closing or stopping a configured I2C monitor sends that channel's stop command before the host session exits
6. SPI monitor sessions are bus-owned, so stopping one logical channel tears down the whole bus group and restarts any remaining sibling monitor windows, while reconfiguring one logical channel reapplies one new shared bus configuration across all surviving sibling channels before those windows are restarted
7. stop commands are cleanup-only and are treated as best-effort, so a stop failure does not block local process teardown or final app exit

## Current Limits

The current app is intentionally small.

- it is a terminal operator tool, not a full analyzer UI
- it prints decoded packets as text rather than rendering protocol-specific tables or waveforms
- channel filtering is still host-side filtering over the full trace stream
- detached monitor windows are currently implemented for Windows consoles

The current automated test entry point is `pytest -q` from `host/python/`.

If future work needs richer analysis or lower USB bandwidth, that should be built on top of the existing control and trace library boundaries rather than folded into the CLI itself.