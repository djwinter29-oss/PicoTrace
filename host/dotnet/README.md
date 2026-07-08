# .NET Host Library

This folder contains a minimal .NET bulk-capture console app in `PicoTrace/`.

It also reserves `PicoTrace.Tests/` for future unit tests.

It targets the current firmware USB vendor interface:

- VID: `0xCAFE`
- PID: `0x4003`
- bulk IN endpoint: `0x83`

## Prerequisites

- .NET 8 SDK or newer
- a libusb-compatible driver/backend for the vendor interface

Platform notes:

- Windows: bind the vendor bulk interface to WinUSB or libusb with Zadig.
- Linux: install libusb and ensure your user has permission to access the device.

## Run

```bash
dotnet run --project host/dotnet/PicoTrace -- --duration 5
```

Example output:

```text
total bytes read: 123456
average speed: 24691.20 B/s
```

The default host read size is tuned for throughput. You can still override it if you want to experiment:

```bash
dotnet run --project host/dotnet/PicoTrace -- --read-size 16384
```
