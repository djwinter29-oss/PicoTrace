# Python Host Library

This package contains a minimal bulk-capture helper for PicoTrace.
It publishes as `picotrace` and exposes the `picotrace-capture` CLI.

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

The capture helper only claims the USB interface that owns bulk IN endpoint `0x83`. It does not try to claim the CDC or HID interfaces, which keeps the tool usable on both Windows and Linux for this composite device.

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

## Capture for 5 seconds

```bash
picotrace-capture
```

Example output:

```text
total bytes read: 123456
average speed: 24691.20 B/s
```

You can override the capture duration if needed:

```bash
picotrace-capture --duration 5
```

The default host read size is tuned for throughput. If you want to experiment, you can still override it:

```bash
picotrace-capture --read-size 16384
```
