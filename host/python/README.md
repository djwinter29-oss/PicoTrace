# Python Host Library

This package contains the PicoTrace Python host library and CLI.

Implementation-level design notes for this package live under [docs/details/](docs/details/README.md).

It also includes a small host-side decode library for the vendor bulk trace stream:

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
- keep the main control console available on Windows while monitors run in separate windows
- fall back to foreground streaming on platforms without detached monitor consoles
- continuously decode and print matching channel traffic

Run the CLI app with either subcommands or the interactive menu:

```bash
picotrace --help
picotrace
```

In interactive mode on Windows, reconfiguring a logical channel ends that channel's current monitor session first, then starts a new monitor window for the new session. Closing or stopping a configured I2C monitor sends that channel's stop command before the host session exits. SPI monitors are bus-owned: stopping one logical SPI channel tears down the whole bus group and restarts any remaining sibling monitor windows, while reconfiguring one logical SPI channel reapplies one new shared bus configuration across all surviving sibling channels before those monitor windows are restarted. On platforms without detached monitor consoles, the interactive command falls back to foreground streaming in the current console.

Monitor shutdown is intentionally best-effort. The host always tries to send the matching I2C or SPI stop command, but it does not treat a stop failure as fatal during window close, foreground stream exit, or final application shutdown. In practice, the monitor may already be gone, the device may have been unplugged, or the previous session may already be over. The CLI treats stop commands as cleanup, not as a second critical control transaction that must succeed before the host can exit cleanly.

Channel selection is also intentionally lightweight on the host side. The trace path uses the requested channel as a filter over decoded packets, so a channel with no matching traffic simply prints nothing. The CLI avoids adding extra host-only policy for that case and relies on the existing device and stream behavior instead.

For workflows that already use another control path, the CLI also supports `trace --all`. That path is a passive bulk-only monitor: it skips channel registration entirely, assumes configuration comes from another interface such as the CDC CLI, and stays open idling until you stop it with `Ctrl+C`.

Run the Python host tests with:

```bash
pytest -q
```

It targets the current firmware USB vendor interface:

- VID: `0xCAFE`
- PID: `0x4003`
- bulk IN endpoint: `0x83`

## Prerequisites

- Python 3.9+
- libusb-1.0 available to PyUSB

Platform notes:

- Windows: install a libusb-compatible backend and make sure `libusb-1.0.dll` is available.
- Linux: install libusb and run with udev permissions for the device, or run once with elevated privileges to confirm access.

The trace transport only claims the USB interface that owns bulk IN endpoint `0x83`. It does not try to claim the CDC or HID interfaces, which keeps the tool usable on both Windows and Linux for this composite device.

## Install

```bash
pip install -e .
```

## Create a virtual environment

Windows:

```powershell
.\tools\windows\host_python_venv.ps1
```

Linux:

```bash
./tools/linux/host_python_venv.sh
```

Both scripts create `.venv/`, upgrade `pip`, and install `requirements.txt`.

The shared requirements file is:

```text
requirements.txt
```

Linux example dependencies:

```bash
sudo apt-get install libusb-1.0-0
```

## Decode library

Decode one raw trace packet:

```python
from picotrace.trace import decode_trace_packet, decode_i2c_events

packet = decode_trace_packet(raw_packet_bytes)
if packet.header.trace_type.name == "I2C":
	events = decode_i2c_events(packet)
```

Decode a continuous bulk stream incrementally:

```python
from picotrace.trace import TraceStreamDecoder

decoder = TraceStreamDecoder()
for packet in decoder.append(chunk_from_usb):
	print(packet.header.channel, packet.header.sequence)
```

Register channels and filter packets before handing them to the caller:

```python
from picotrace.trace import TraceChannelRegistry, iter_trace_packets

registry = TraceChannelRegistry()
registry.register_channel(1)
registry.register_channel(3)

for packet in iter_trace_packets(duration_seconds=5, channel_registry=registry):
	print(packet.header.channel, packet.header.sequence)
```

Open the PicoTrace bulk device from the trace module and read decoded packets:

```python
from picotrace.trace import iter_trace_packets

for packet in iter_trace_packets(duration_seconds=5):
	print(packet.header.trace_type, packet.header.payload_len)
```

Stream one logical channel or the full decoded trace stream from the CLI without changing device configuration:

```bash
picotrace trace --channel 4
picotrace trace --all
```

## HID control library

Open the PicoTrace HID control interface and query the shared device status:

```python
from picotrace.control import HidControlClient

with HidControlClient.open() as control:
	status = control.get_status()
	print(status.stream_enabled)
```

Set one I2C channel sample rate:

```python
from picotrace.control import HidControlClient

with HidControlClient.open() as control:
	control.i2c_set_rate(channel=0, sample_hz=1_000_000)
```

Configure one observed SPI bus:

```python
from picotrace.control import HidControlClient
from picotrace.trace import SpiCaptureMode

with HidControlClient.open() as control:
	control.spi_set_config(
		bus=0,
		capture=SpiCaptureMode.MOSI_MISO,
		spi_mode=0,
		channel_select_mask=0x03,
		timeout_us=100,
	)
```
