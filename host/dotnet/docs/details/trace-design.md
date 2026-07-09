# Trace Design

## Purpose

The `PicoTrace.Trace` namespace provides the host-side path for consuming PicoTrace trace packets.

Today that means:

- decode fixed packet headers and protocol payloads
- frame a continuous byte stream into trace packets
- read trace packets from the current USB vendor bulk IN transport
- optionally filter packets by logical channel before yielding them to the caller

The design keeps decode separate from transport so the same framing and protocol logic can be reused if the byte source moves from USB bulk to a file, CDC, or another stream source.

## Current Module Layout

- `Trace/TraceModels.cs`: packet, event, capture-mode, and exception types
- `Trace/TraceDecoder.cs`: transport-agnostic packet framing and protocol decode
- `Trace/TraceChannelRegistry.cs`: channel registration and packet filtering
- `Trace/UsbBulkTraceTransport.cs`: current USB vendor bulk transport

## Packet Contract

The host decode path follows the current firmware packet contract in `firmware/src/trace/trace_packet.h`.

- fixed packet header size: 16 bytes
- maximum packet size: 128 bytes
- streamed wire shape: `header + payload_len`
- packet fields include:
  - version
  - trace type
  - logical channel
  - flags
  - payload length
  - protocol metadata
  - sequence
  - timestamp

The current .NET host understands two trace types:

- I2C
- SPI

## Decode Layer

`TraceDecoder` owns the pure protocol logic.

Key responsibilities:

- `DecodeTracePacket(...)` validates one packet-sized byte span
- `TraceStreamDecoder` incrementally frames a continuous stream of bytes into packets
- `DecodeI2cEvents(...)` interprets the I2C payload contract
- `DecodeSpiSamples(...)` interprets the SPI payload contract

The decode layer does not know where bytes come from.

That is intentional. It keeps the packet contract reusable across:

- USB bulk streaming
- future CDC streaming
- recorded trace files
- test fixtures

## Filter Layer

`TraceChannelRegistry` provides a host-side filter over the existing stream.

Current behavior:

- when no channels are registered, all packets pass through
- when channels are registered, only packets whose `Header.Channel` is in the registry are yielded

This is a consumer-side filter, not a firmware-side subscription model.

The `.NET` CLI uses this directly for two trace workflows:

- `trace --channel <n>` registers one channel and prints only matching packets
- `trace --all` leaves the registry empty and prints the full decoded stream

That means:

- USB bandwidth is unchanged
- the device still emits the full stream
- the .NET library filters packets before handing them to the caller

If real transport bandwidth reduction is needed later, that should be a separate firmware control feature rather than an extension of the current host-side filter.

## Transport Layer

`UsbBulkTraceTransport` owns the current USB vendor bulk IN path.

Responsibilities:

- locate the PicoTrace USB device through `LibUsbDotNet`
- find and claim the vendor bulk interface
- keep the owning `UsbContext` alive for the full claimed-device lifetime
- read raw bytes from endpoint `0x83`
- feed those bytes into `TraceStreamDecoder`
- optionally apply `TraceChannelRegistry` filtering before yielding packets

The transport layer stays focused on device access and byte movement. It does not own protocol-specific payload formatting beyond what is necessary to construct complete `TracePacket` objects.

## Extension Direction

The preferred direction for future growth is to add sibling modules beside the current bulk transport rather than widen one transport file.

Examples:

- a replay/file transport for offline decode
- a CDC transport if the firmware grows one later

Those transport modules should feed the same `TraceStreamDecoder` and yield the same `TracePacket` objects.