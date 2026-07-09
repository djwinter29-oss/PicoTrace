# Trace Design

## Purpose

The `picotrace.trace` package provides the host-side path for consuming PicoTrace trace packets.

Today that means:

- decode fixed packet headers and protocol payloads
- frame a continuous byte stream into trace packets
- read trace packets from the current USB vendor bulk IN transport
- optionally filter packets by logical channel before yielding them to the caller

The design keeps decode separate from transport so the same framing and protocol logic can be reused if the byte source moves from USB bulk to CDC, a file, or another stream source.

## Current Module Layout

- `trace/decode.py`: transport-agnostic packet framing and protocol decode
- `trace/filter.py`: channel registration and packet filtering
- `trace/usb_bulk.py`: current USB vendor bulk transport
- `trace/__init__.py`: stable public surface for the trace package

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

The host package currently understands two trace types:

- I2C
- SPI

## Decode Layer

`trace/decode.py` owns the pure protocol logic.

Key responsibilities:

- `decode_trace_packet(...)` validates one packet-sized byte string
- `TraceStreamDecoder` incrementally frames a continuous stream of bytes into packets
- `decode_i2c_events(...)` interprets the I2C payload contract
- `decode_spi_samples(...)` interprets the SPI payload contract

The decode layer does not know where bytes come from.

That is intentional. It keeps the packet contract reusable across:

- USB bulk streaming
- future CDC streaming
- recorded trace files
- test fixtures

## Filter Layer

`trace/filter.py` provides `TraceChannelRegistry`.

The registry is a host-side filter over the existing stream.

Current behavior:

- when no channels are registered, all packets pass through
- when channels are registered, only packets whose `header.channel` is in the registry are yielded

This is a consumer-side filter, not a firmware-side subscription model.

That means:

- USB bandwidth is unchanged
- the device still emits the full stream
- the Python library filters packets before handing them to the caller

If real transport bandwidth reduction is needed later, that should be a separate firmware control feature rather than an extension of the current host-side filter.

## Transport Layer

`trace/usb_bulk.py` owns the current USB vendor bulk IN path.

Responsibilities:

- locate the PicoTrace USB device through the shared libusb backend helper
- find and claim the vendor bulk interface
- read raw bytes from endpoint `0x83`
- feed those bytes into `TraceStreamDecoder`
- optionally apply `TraceChannelRegistry` filtering before yielding packets

The transport layer should stay focused on device access and byte movement. It should not own protocol-specific payload decoding beyond what is necessary to construct complete `TracePacket` objects.

## Public Surface

`trace/__init__.py` re-exports the current trace library surface so callers can use `picotrace.trace` without depending on internal file layout.

That surface currently includes:

- packet and enum types
- decode helpers
- stream decoder
- channel registry
- bulk transport helpers

## Extension Direction

The preferred direction for future growth is to add sibling modules beside the current bulk transport rather than widen one transport file.

Examples:

- `trace/cdc.py` for CDC transport
- `trace/file.py` for replay or offline decode

Those transport modules should feed the same `TraceStreamDecoder` and yield the same `TracePacket` objects.