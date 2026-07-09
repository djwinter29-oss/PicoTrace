# Control Design

## Purpose

The `PicoTrace.Control` namespace provides host-side access to the PicoTrace control channel.

Today the control path is implemented over USB HID and is intended for bounded device control, not high-throughput trace data.

Current responsibilities:

- build and validate fixed-size HID command and response reports
- send commands over the HID control interface
- decode typed status payloads for the current I2C and SPI monitor controls

## Current Module Layout

- `Control/HidProtocol.cs`: pure HID framing, payload builders, and payload decoders
- `Control/HidControlClient.cs`: USB HID transport and request flow

## HID Contract

The .NET host follows the current firmware contract in:

- `firmware/src/usb/usb_hid.h`
- `firmware/src/usb/usb_hid.c`
- `firmware/src/usb/usb_descriptors.c`

Current protocol properties:

- fixed report size: 64 bytes
- report layout:
  - opcode
  - sequence
  - status
  - payload length
  - payload bytes
- current transport path:
  - HID class `SET_REPORT` for outbound commands
  - HID class `GET_REPORT` for inbound responses

## Protocol Layer

`HidProtocol` owns all logic that does not require a live USB device.

Key responsibilities:

- define HID opcodes and status enums
- pack one outbound HID command into a 64-byte report
- decode one inbound HID response from a 64-byte report
- build typed payloads for supported commands
- validate builder inputs before they are narrowed to one-byte protocol fields
- decode typed payloads for supported status responses

This separation matters because the command and status contract should remain reusable if the same control messages are later carried over a different transport.

## HID Transport Layer

`HidControlClient` owns the current USB device interaction.

Responsibilities:

- find the PicoTrace device through the shared `LibUsbDotNet` helper path
- locate the HID control interface and claim it
- keep the owning `UsbContext` alive for the full claimed-device lifetime
- issue HID class control transfers
- match responses by opcode and sequence
- expose a small client API for common control operations

The transport layer stays focused on device interaction and request flow. It should not duplicate payload parsing logic that already belongs in the protocol layer.

## Current Client Surface

`HidControlClient` provides the main user-facing API.

Current operations include:

- shared device status query
- stream enable and disable
- LED control
- reboot request
- I2C monitor set-rate and status queries
- SPI monitor set-config and status queries

The client uses typed protocol helpers under the hood and returns typed status objects instead of raw payload bytes.

## Design Boundaries

The current control namespace is intentionally narrower than the trace namespace.

- control is command-oriented and bounded
- trace is stream-oriented and throughput-sensitive

The control namespace should not become a second data-stream path. It should stay centered on device configuration, status, and small control transactions.

## Extension Direction

If additional control transports are needed later, the preferred shape is:

- keep `HidProtocol.cs` transport-agnostic
- add sibling transport modules beside `HidControlClient.cs`

Those transport modules should reuse the same protocol helpers rather than inventing parallel command packing or status decoding logic.