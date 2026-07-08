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

The current implementation now includes a concrete SPI trace packet contract for the shared ring
and USB stream path. The design notes below describe that current contract rather than leaving it
open.

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

## Current `CS_N` Attribution Model

The current PicoTrace SPI monitor is intentionally optimized for the common controller-driven SPI
transaction model:

- one asserted `CS_N` usually corresponds to one logical transaction
- `CS_N` changes usually occur at a transaction boundary
- that boundary usually comes with a visible clock pause or idle gap

That is the situation PicoTrace is currently designed around.

The present implementation samples bus data continuously but attributes `CS_N` ownership at DMA
half-buffer handoff rather than on every sampled SPI bit. In the common case above, that is an
acceptable simplification because the next transaction normally starts after the `CS_N` boundary,
so the handoff view still matches the logical transaction split seen by the controller.

## Current Design Limitation

PicoTrace does not currently guarantee exact attribution for the corner case where:

- `CS_N` changes inside one DMA half-buffer
- the controller does not leave a reliable clock gap at that boundary
- meaningful clocked data continues closely enough that half-buffer attribution can merge or
  mis-assign bytes around the transition

This corner case is intentionally out of scope for the current SPI monitor design.

That tradeoff keeps the monitor small and aligned with PicoTrace's main target: low-cost passive
capture of common SPI traffic patterns rather than exhaustive support for every possible master-side
timing behavior.

If future target systems require exact attribution across back-to-back `CS_N` changes without a
usable idle gap, the correct upgrade path is to sample `CS_N` history directly in the raw capture
stream and decode those transitions in software instead of inferring ownership only at buffer
handoff.

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

## Current SPI Packet Contract

The current SPI producer emits `TRACE_TYPE_SPI` packets into the shared `trace_ring`.

The packet header fields currently mean:

- `type = TRACE_TYPE_SPI`
- `channel =` the logical SPI channel selected by the active observed `CS_N`
- `flags` use the shared trace-packet meanings:
  - `TRACE_FLAG_END` closes a logical transaction
  - `TRACE_FLAG_CONTINUED` marks a fragment that continues an earlier transaction
  - `TRACE_FLAG_OVERFLOW` marks a fragment that follows dropped output
  - `TRACE_FLAG_TRUNCATED` marks a transaction that ended with a partial byte in progress
  - `TRACE_FLAG_ERROR` marks an error boundary such as conflicting active `CS_N` selection
- `payload_len =` the number of valid payload bytes in the fragment
- `meta =` the configured capture mode for that session:
  - `SPI_MONITOR_CAPTURE_MOSI` for MOSI-only capture
  - `SPI_MONITOR_CAPTURE_MOSI_MISO` for dual-lane capture
- `sequence =` the per-logical-channel fragment counter for the current monitor session
- `timestamp_us =` the producer timestamp recorded when the fragment is opened

The payload format is currently:

- MOSI-only capture: one payload byte per observed SPI byte
- MOSI+MISO capture: interleaved byte pairs in transaction order
  - payload byte `0` = MOSI byte `0`
  - payload byte `1` = MISO byte `0`
  - payload byte `2` = MOSI byte `1`
  - payload byte `3` = MISO byte `1`

This is the current implemented contract. Host-side decode should treat `meta` as the lane-mode
selector that tells it whether payload bytes are MOSI-only or MOSI/MISO interleaved pairs.

That interleaved dual-lane layout is a delivery format, not a timing guarantee. PicoTrace's
current SPI monitor is content-first: in `SPI_MONITOR_CAPTURE_MOSI_MISO` mode it aims to deliver
both MOSI and MISO content for the same `CS_N`-owned logical transaction, but it does not promise
that the two physical lanes were buffer-synchronized or phase-aligned at every byte boundary.
Host software should therefore treat the payload as two delivered directional streams encoded into
one packet format, not as proof of cycle-accurate MOSI/MISO lockstep.

## Timeout Rule

The timeout exists only to terminate a stalled or abandoned transaction when no explicit `CS_N`
change arrives soon enough.

The exact timeout value does not need to be fixed yet, but the implementation should treat it as a
capture boundary equivalent to end-of-transaction for packetization purposes.

## Lane Selection Semantics

This design fixes the monitor start modes and the current emitted packet format:

- MOSI-only monitoring must capture controller-to-peripheral data
- MOSI+MISO monitoring must capture both directions for the same transaction

The feature goal for MOSI+MISO mode is delivery completeness, not lane synchronization. The
monitor intentionally does not require MOSI and MISO DMA half-buffers to retire in lockstep before
forwarding decoded content to the user. That keeps the producer path simple and aligned with the
current PicoTrace target: low-cost passive capture where getting both directions to the host is
more important than proving exact cross-lane timing alignment.

The current implementation represents MOSI+MISO data as interleaved byte pairs in the payload and
uses `trace_packet_header_t.meta` to carry the active capture mode so the host can distinguish the
layout. Consumers that need exact per-byte pairing or stronger cross-lane timing guarantees should
treat that as out of scope for the current design.

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