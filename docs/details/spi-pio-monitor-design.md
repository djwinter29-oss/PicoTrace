# SPI PIO Monitor Design

## Purpose

This document describes the current intended firmware design for passive SPI monitoring and trace
packetization.

The focus here is the capture boundary from SPI clocked sampling into logical trace packets that
can be pushed into the shared trace ring.

## Related Architecture

For the higher-level component view around this design, see:

- [../architecture/firmware-architecture.md](../architecture/firmware-architecture.md)
- [../architecture/interface-and-synchronization.md](../architecture/interface-and-synchronization.md)

## Scope

This design covers the SPI capture implementation scaffold under `firmware/src/trace/capture/`.

This design does not yet lock down:

- the final SPI payload byte layout
- the final meaning of `trace_packet_header_t.meta` for SPI
- whether MOSI and MISO are emitted as one combined payload stream or two independent logical
  payload streams

Those format details should be finalized only when the SPI packet contract is nearly complete.

## Startup State

At boot, SPI monitoring is disabled on all SPI logical channels.

No SPI sampler should run until the control path explicitly starts monitoring for a logical SPI
channel.

## Control Model

Each observed SPI bus should support three monitor states:

- stopped
- MOSI-only monitoring
- MOSI+MISO monitoring

Each start request must also select one of the four standard SPI modes:

- mode 0
- mode 1
- mode 2
- mode 3

Changing monitoring mode, lane selection, or SPI mode should use the same destructive stop-first
restart policy already used by the I2C monitor. Any in-flight raw samples, partial packet state,
or pending transaction bytes for that running session may be discarded at reconfiguration time.

The control boundary is per observed SPI bus, not per logical channel. Each start request must
also choose which observed `CS_N` slots on that bus are eligible for capture:

- one specific channel on that bus
- all three observed channels on that bus

This prevents a floating or unconnected active-low `CS_N` input from permanently looking selected
and contaminating packet ownership for unrelated traffic on the same bus.

## Hardware Mapping Assumption

The current board mapping exposes two observed SPI buses and three observed `CS_N` inputs per bus
as documented in `docs/hardware-connections.md`.

Important implication:

- `SCLK`, `MOSI`, and optional `MISO` are bus-level signals
- `CS_N` selects the logical channel within that bus

That means the capture implementation should treat SPI sampling as bus-owned, then route completed
transaction bytes to the logical channel identified by the active `CS_N` input.

The current logical channel map is:

- `0x00` to `0x02` for `spi0` `cs_slot` 0 to 2
- `0x03` to `0x05` for `spi1` `cs_slot` 0 to 2

## Capture Trigger

PIO sampling should be clocked from the observed SPI clock.

The SPI clock, not a free-running oversample rate, is the capture trigger for the SPI sampler.

The implementation should interpret edges according to the selected SPI mode so that the sampler
uses the correct shift edge and capture edge for modes 0 through 3.

## Bus Ownership Model

The natural ownership boundary is one sampler per observed SPI bus, not one sampler per logical
channel.

Reason:

- all logical channels on one observed SPI bus share the same `SCLK`
- all logical channels on one observed SPI bus share the same `MOSI`
- optional `MISO` is also shared at the bus level
- only `CS_N` distinguishes which logical channel owns a transaction

The future implementation should therefore maintain per-bus capture state and per-logical-channel
trace packet state.

## Transaction Boundary Rules

The active `CS_N` line determines which logical channel a sampled SPI byte belongs to.

The transaction should end when either of these conditions occurs:

- the active `CS_N` deasserts
- another observed `CS_N` becomes active
- an inter-byte timeout expires while a transaction is open

For this design, a `CS_N` change or timeout is treated as the end of the current logical packet.

If more than one observed `CS_N` is active at once, that should be treated as an error boundary and
the current packet should be closed before capture resumes from the next clean selection state.

## Packet Assembly Rules

Each logical SPI channel should own a persistent open `trace_packet_t` assembly buffer for the
current transaction.

When sampled bytes are attributed to a logical channel:

- append the received SPI data to that channel's open packet
- keep the packet open while the same transaction continues
- push the packet when the transaction ends

If the open packet reaches `TRACE_PACKET_PAYLOAD_BYTES`, the implementation should immediately
dispatch that fragment into the trace ring and continue the same transaction in a new fragment.

That matches the current fixed-packet ring model and avoids requiring a larger transient SPI
transaction buffer.

## Timeout Rule

The timeout exists only to terminate a stalled or abandoned transaction when no explicit `CS_N`
change arrives soon enough.

The exact timeout value does not need to be fixed yet, but the implementation should treat it as a
capture boundary equivalent to end-of-transaction for packetization purposes.

## Lane Selection Semantics

This design fixes the monitor start modes but intentionally leaves the emitted packet format open:

- MOSI-only monitoring must capture controller-to-peripheral data
- MOSI+MISO monitoring must capture both directions for the same transaction

The remaining open design question is only how those bytes are represented in the trace payload and
how the host distinguishes the directions from the header. That decision should be made together
with the final SPI packet format.

## Sequence And Timestamp Intent

Each emitted SPI packet fragment should fill the shared trace packet header the same way the I2C
path does now:

- `type` identifies SPI
- `channel` identifies the logical SPI channel selected by `CS_N`
- `sequence` increments per logical channel session
- `timestamp_us` records when the fragment is opened

If SPI monitoring is stopped and restarted for a channel, that should be treated as a new session.

## Error Handling Intent

The SPI monitor should follow the same broad producer policy used elsewhere in PicoTrace:

- preserve capture flow over perfect retention
- drop newest output if the shared trace ring cannot accept a completed fragment
- mark later successful output so the host can detect loss

Candidate error boundaries include:

- ring push failure
- conflicting `CS_N` selection
- internal sampler overrun
- forced stop for control reconfiguration

## Why This Design Fits PicoTrace

This is the smallest SPI monitor design that fits the current PicoTrace architecture:

- disabled by default at boot
- explicit host-controlled start and stop
- shared bus capture with logical-channel routing by `CS_N`
- packet termination on `CS_N` change or timeout
- immediate fragment push at `TRACE_PACKET_PAYLOAD_BYTES`
- no requirement for a large per-transaction temporary buffer