# USB Multi-Interface Design

## Purpose

This document describes how PicoTrace uses one USB device for multiple concurrent roles without
adding separate physical links or protocol-specific side channels.

The current firmware uses TinyUSB to expose three different host-visible USB functions at the same
time:

- CDC for the board-local text CLI
- vendor bulk for high-rate trace streaming
- HID for bounded structured control and status

## Related Architecture

For the higher-level component view around this design, see:

- [../architecture/firmware-architecture.md](../architecture/firmware-architecture.md)
- [../architecture/interface-and-synchronization.md](../architecture/interface-and-synchronization.md)

## Design Goal

The USB design tries to satisfy two competing needs:

- stream trace data continuously with low software overhead
- keep a simple control path available even while streaming is active

The chosen design keeps all three functions on one USB device and one TinyUSB stack, but assigns
each function a narrow role so the interfaces do not compete semantically even though they still
share the same USB bus.

## Host-Visible Functions

The configuration descriptor in [firmware/src/usb/usb_descriptors.c](c:/repo/Pico/PicoTrace/firmware/src/usb/usb_descriptors.c) exposes these interfaces:

- CDC ACM interface pair for interactive command and debug text
- vendor interface for binary trace data movement
- HID interface for fixed-size command and response packets

The interface strings currently identify them as:

- `CDC Host Control`
- `Trace Data Stream`
- `HID Control`

This split is intentional. Human-oriented command traffic and machine-oriented control traffic do
not share the same framing rules, and the continuous trace stream stays isolated from both.

## Why Three USB Functions Exist

### CDC

CDC exists for a board-local shell experience. It is the easiest interface for a developer to open
with a terminal and use manually during bring-up, lab work, and debug.

The CDC path is buffered in [firmware/src/usb/usb_cdc.c](c:/repo/Pico/PicoTrace/firmware/src/usb/usb_cdc.c) and is bridged into the CLI shell from [firmware/src/main.c](c:/repo/Pico/PicoTrace/firmware/src/main.c).

CDC is not the main trace transport. It is intentionally treated as bounded command traffic.

### Vendor Bulk

Vendor bulk exists for the real trace payload path. It is the only interface meant to carry the
shared trace-ring packet stream to the host.

The vendor stream path in [firmware/src/usb/usb_bulk.c](c:/repo/Pico/PicoTrace/firmware/src/usb/usb_bulk.c):

- peeks packets from the shared trace ring
- preserves packet boundaries while sending them
- resumes partial writes when endpoint space is limited
- drops invalid packets instead of stalling the stream path

On Windows, the vendor interface is additionally advertised through the Microsoft OS 2.0
descriptor set as WinUSB so host software can bind to it without a custom kernel driver.

### HID

HID exists for small structured control transactions that should not depend on a terminal session
or text parsing.

The HID path in [firmware/src/usb/usb_hid.c](c:/repo/Pico/PicoTrace/firmware/src/usb/usb_hid.c) carries fixed-size commands for operations such as:

- stream enable and disable
- I2C monitor control and status
- SPI monitor control and status
- LED control
- reboot

HID is kept bounded on purpose. It is a control path, not a second streaming channel.

## Core Ownership Model

TinyUSB and all USB endpoint service stay on core 0.

Protocol monitor production stays on core 1.

That ownership split is visible in [firmware/src/main.c](c:/repo/Pico/PicoTrace/firmware/src/main.c):

- core 0 runs `tud_task()` and services CDC, HID, and vendor traffic
- core 1 initializes capture scaffolds and executes monitor-control mailboxes

This matters because CDC, HID, and vendor bulk are not independent buses. They are different
interfaces on the same USB device stack, so splitting endpoint ownership across cores would add
coordination complexity without creating more bus bandwidth.

## Poll And Service Order

The current foreground loop on core 0 uses a stream-first service order:

1. run `tud_task()`
2. if the device is ready, service vendor trace streaming multiple times
3. periodically service CLI input, CDC transmit flush, and HID commands

This ordering is intentional.

The trace stream is the highest-throughput path and is the most sensitive to starvation. CDC and
HID are both bounded control channels, so they can tolerate being serviced less often than the bulk
stream as long as they remain responsive.

The constants in [firmware/src/main.c](c:/repo/Pico/PicoTrace/firmware/src/main.c) currently express that policy:

- `STREAM_SERVICE_PASSES` gives the vendor stream several opportunities per loop
- `STREAM_CONTROL_POLL_DIVIDER` throttles CDC and HID service relative to stream service

## Data And Control Separation

The multi-interface design works because the responsibilities are separated clearly:

- trace packets only leave through vendor bulk
- human CLI text only flows through CDC
- structured device control only flows through HID

This avoids two common failure modes:

- trying to stream binary trace data through a text-oriented console transport
- letting ad hoc debug control traffic contend directly with the main trace data path

## Backpressure Behavior

### Vendor Stream Backpressure

The vendor stream code only writes what the endpoint can currently accept. If the endpoint cannot
accept more bytes, the current packet remains borrowed until the next poll.

If streaming is disabled through [firmware/src/app_control.c](c:/repo/Pico/PicoTrace/firmware/src/app_control.c), the partial vendor-stream state is dropped and the next enable restarts from a packet boundary.

### CDC Backpressure

CDC writes are staged through a local transmit queue. If there is no room in the queue, the write
request fails rather than blocking the main loop.

That keeps the text shell from monopolizing foreground execution.

### HID Backpressure

HID uses one pending command slot and one prepared response slot. That is enough for the current
bounded command model and avoids building a richer deferred command system before it is needed.

## Why This Fits PicoTrace

This multi-interface USB design fits PicoTrace because it stays aligned with the current product
shape:

- one physical USB connection
- one TinyUSB device stack
- one high-throughput trace stream path
- two bounded control paths with different ergonomics
- no protocol-specific side transport for SPI versus I2C

It keeps the streaming path simple, preserves an interactive lab-friendly CLI, and gives host tools
a structured non-text control mechanism without complicating the capture architecture.

## Current Limits

The USB functions are logically separate, but they still share one USB device controller and one
bus.

That means:

- CDC, HID, and vendor bulk still compete for service time on core 0
- HID is not a zero-cost out-of-band control plane
- CDC should not be treated as a high-rate telemetry path
- future throughput work should improve stream scheduling and buffering before adding more USB roles

## Future Work Boundaries

If the trace stream grows more demanding, the preferred evolution is still within the same model:

- keep USB ownership on one core
- improve producer-to-stream buffering and packet handoff
- keep HID and CDC bounded
- avoid adding protocol-specific extra interfaces unless a concrete host requirement justifies them

That preserves the current PicoTrace architecture while leaving room to tune throughput later.