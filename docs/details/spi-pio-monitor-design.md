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

This design covers the SPI capture implementation scaffold under `firmware/src/trace/capture/spi/`.

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

Changing monitoring mode, capture direction, or SPI mode should use the same destructive stop-first
restart policy already used by the I2C monitor. Any in-flight raw samples, partial packet state,
or pending transaction bytes for that running session may be discarded at reconfiguration time.

The control boundary is per observed SPI bus, not per logical channel. Each start request must
also choose which observed `CS_N` slots on that bus are eligible for capture:

- one specific channel on that bus
- all two observed channels on that bus

This prevents a floating or unconnected active-low `CS_N` input from permanently looking selected
and contaminating packet ownership for unrelated traffic on the same bus.

## Hardware Mapping Assumption

The current board mapping exposes two observed SPI buses and two observed `CS_N` inputs per bus
as documented in `docs/hardware-connections.md`.

Important implication:

- `SCLK`, `MOSI`, and optional `MISO` are bus-level signals
- `CS_N` selects the logical channel within that bus

That means the capture implementation should keep bus-wide control semantics but treat capture
ownership as `bus + cs_slot`, not as one undifferentiated bus sampler.

The current logical channel map is:

- `0x00` to `0x01` for `spi0` `cs_slot` 0 to 1
- `0x02` to `0x03` for `spi1` `cs_slot` 0 to 1

## Capture Trigger

PIO sampling should be clocked from the observed SPI clock.

The SPI clock, not a free-running oversample rate, is the capture trigger for the SPI sampler.

The implementation should interpret edges according to the selected SPI mode so that the sampler
uses the correct shift edge and capture edge for modes 0 through 3.

## Capture Ownership Model

The updated ownership boundary is one sampler per observed `CS_N` slot, not one sampler per whole
observed SPI bus.

Reason:

- same-bus multi-channel capture needs exact ownership at the moment the target `CS_N` is active
- `SCLK`, `MOSI`, and optional `MISO` are still shared electrically at the bus level
- `CS_N` is the only valid hardware gate that says whether one logical channel should capture data
- continuous bus-owned sampling followed by later `CS_N` attribution is no longer sufficient for
  the target behavior

The intended implementation should therefore maintain:

- bus-wide control state for `spi_mode`, capture direction, timeout, and enablement policy
- one PIO state machine per observed `CS_N` slot
- one DMA ring per observed `CS_N` slot
- one active packet builder per observed `CS_N` slot

For the current RP2040 target, this design is intentionally paired with only two `CS_N` slots per
observed bus so the state-machine budget remains practical.

## CS Gating Rule

The sampler must not shift any SPI data unless its own observed `CS_N` input is active.

That rule is mandatory for the updated design.

More concretely:

- when a sampler's `CS_N` is deasserted, that sampler must remain idle
- `SCLK` edges seen while that `CS_N` is deasserted must not generate sampled words for that stream
- a sampler may begin shifting only after its own `CS_N` becomes active
- a sampler must stop shifting immediately when its own `CS_N` deasserts

This is the key design change relative to the earlier bus-owned continuous sampler model.

The intended PIO behavior is therefore `CS_N`-gated and `SCLK`-clocked:

- `CS_N` active is the admission condition
- `SCLK` edges define when bits are shifted
- `MOSI` and optional `MISO` remain the sampled payload inputs

## Transaction Boundary Rules

Each observed `CS_N` slot owns its own logical SPI stream.

The transaction should end when either of these conditions occurs:

- that stream's `CS_N` deasserts
- an inter-byte timeout expires while a transaction is open

For this design, `CS_N` deassertion or timeout is treated as the end of the current logical packet.
When the observed `CS_N` is idle, the current logical transaction for that stream is considered
finished and the monitor should close and emit the accumulated packet fragment sequence for that
transaction.

If more than one observed `CS_N` is active at once, that is no longer a bus-ownership conflict in
this design because each observed stream owns its own sampler and DMA ring.

In that case:

- each active selected stream may continue to sample while its own `CS_N` remains asserted
- `CS_N` deassertion still closes only that stream's transaction
- overlap does not by itself require `TRACE_FLAG_ERROR`

## Redesign Motivation

The earlier bus-owned continuous sampler model was optimized for the common case where one asserted
`CS_N` maps cleanly to one transaction and there is enough idle gap to infer ownership at DMA
handoff.

That is no longer the target design.

The updated design explicitly removes that dependency by ensuring that each logical SPI stream only
captures data while its own `CS_N` is active. This avoids the earlier ambiguity where back-to-back
same-bus `CS_N` changes could merge or mis-assign bytes inside one DMA half-buffer.

## Packet Assembly Rules

The active transaction on one observed `bus + cs_slot` stream should own the live `trace_packet_t`
assembly buffer for the current transaction.

When sampled bytes are attributed to a logical channel:

- append the received SPI data to the active transaction packet currently owned by that logical channel
- keep the packet open while the same transaction continues
- when the current transaction ends, emit the accumulated packet fragment sequence into the shared `trace_ring`
- treat `CS_N` deassertion as one of those end-of-transaction signals

When the current SPI monitor session on that stream is closed normally, such as an explicit stop or
a stop-first reconfiguration while a transaction is open, the monitor should also close and emit the
accumulated fragment sequence for that open transaction before resetting the stream-owned runtime.

If appending more received bytes would grow the current fragment past `TRACE_PACKET_PAYLOAD_BYTES`,
the implementation should immediately emit the current fragment into the shared `trace_ring` and
continue the same transaction in a new fragment.

In other words, `TRACE_PACKET_PAYLOAD_BYTES` is the fixed payload budget for one emitted
`trace_packet_t`. A transaction larger than that budget must be fragmented across multiple emitted
packets instead of waiting for the whole transaction to fit in one buffer.

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
  - `TRACE_FLAG_ERROR` remains reserved for future SPI decode or capture fault signaling, but is
    not used merely because two selected `CS_N` inputs are active at the same time
- `payload_len =` the number of valid payload bytes in the fragment
- `meta =` the configured capture mode for that fragment:
  - `SPI_MONITOR_CAPTURE_MOSI` for MOSI-only capture
  - `SPI_MONITOR_CAPTURE_MOSI_MISO` for MOSI+MISO capture
- `sequence =` the per-logical-channel fragment counter for the current monitor session
- `timestamp_us =` the timestamp recorded when the current logical transaction opened

The payload format is currently:

- MOSI-only capture: one payload byte per observed SPI byte
- MOSI+MISO capture: interleaved byte pairs in transaction order
  - payload byte `0` = MOSI byte `0`
  - payload byte `1` = MISO byte `0`

## Current Sampler Split

The current implementation now uses two different PIO sampler shapes depending on capture mode:

- MOSI-only capture uses a dedicated one-pin sampler that shifts `MOSI` alone and autopushes one
  byte per observed SPI byte time
- MOSI+MISO capture keeps the two-pin sampler that shifts `MOSI` and `MISO` together and
  autopushes one packed raw word per observed SPI byte time

This split keeps the host-visible SPI packet contract unchanged while removing unnecessary two-lane
 sampling and lane-compaction work from the firmware fast path when the user only requested `MOSI`.

The result is intentional asymmetry:

- MOSI-only mode is optimized for the highest practical throughput ceiling
- MOSI+MISO mode preserves the existing two-lane aligned sampler and firmware-side decode path
  - payload byte `2` = MOSI byte `1`
  - payload byte `3` = MISO byte `1`

This is the current implemented contract. Host-side decode should treat `meta` as the capture-mode
selector that tells it whether payload bytes are MOSI-only or MOSI/MISO interleaved pairs.

When a transaction ends, the final fragment for that transaction is emitted to the shared `trace_ring`
with `TRACE_FLAG_END` set. When a transaction is larger than one fixed packet payload, earlier
fragments are emitted as soon as the payload would exceed `TRACE_PACKET_PAYLOAD_BYTES`, and later
fragments continue the same transaction with `TRACE_FLAG_CONTINUED`.

That means there are two normal emission triggers in the current design:

- transaction end, including `CS_N` observed idle at handoff or timeout expiry
- fragment size reaching the fixed `TRACE_PACKET_PAYLOAD_BYTES` limit

That interleaved MOSI+MISO layout is a delivery format, not a timing guarantee. PicoTrace's
current SPI monitor is content-first: in `SPI_MONITOR_CAPTURE_MOSI_MISO` mode it aims to deliver
both MOSI and MISO content for the same `CS_N`-owned logical transaction, but it does not promise
that the two physical signal directions were buffer-synchronized or phase-aligned at every byte boundary.
Host software should therefore treat the payload as two delivered directional streams encoded into
one packet format, not as proof of cycle-accurate MOSI/MISO lockstep.

## Timeout Rule

The timeout exists only to terminate a stalled or abandoned transaction when no explicit `CS_N`
change arrives soon enough.

The exact timeout value does not need to be fixed yet, but the implementation should treat it as a
capture boundary equivalent to end-of-transaction for packetization purposes.

## Capture Direction Semantics

This design fixes the monitor start modes and the current emitted packet format:

- MOSI-only monitoring must capture controller-to-peripheral data
- MOSI+MISO monitoring must capture both directions for the same transaction

The feature goal for MOSI+MISO mode is delivery completeness, not cross-direction synchronization. The
monitor intentionally does not require MOSI and MISO DMA half-buffers to retire in lockstep before
forwarding decoded content to the user. That keeps the producer path simple and aligned with the
current PicoTrace target: low-cost passive capture where getting both directions to the host is
more important than proving exact cross-direction timing alignment.

The current implementation represents MOSI+MISO data as interleaved byte pairs in the payload and
uses `trace_packet_header_t.meta` to carry the active capture mode so the host can distinguish the
layout. Consumers that need exact per-byte pairing or stronger cross-direction timing guarantees should
treat that as out of scope for the current design.

## Sequence And Timestamp Intent

Each emitted SPI packet fragment should fill the shared trace packet header the same way the I2C
path does now:

- `type` identifies SPI
- `channel` identifies the logical SPI channel selected by `CS_N`
- `sequence` increments per logical channel session
- `timestamp_us` records when the logical transaction opened, and all fragments from that same
  transaction keep that same start timestamp

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
- bus-wide control with per-`CS_N` capture ownership
- no sampling when `CS_N` is inactive
- packet termination on `CS_N` deassertion or timeout
- immediate fragment push at `TRACE_PACKET_PAYLOAD_BYTES`
- no requirement for a large per-transaction temporary buffer

## Implementation Status

The checked-in firmware implementation in `firmware/src/trace/capture/spi_monitor.c` and
`firmware/src/trace/capture/spi_monitor.pio` now follows this per-`CS_N` sampler design:

- two observed `CS_N` slots per bus
- one sampler and DMA path per `CS_N` slot
- no sampling while `CS_N` is inactive

Treat the current implementation and this document as the same design baseline. If they diverge in
future changes, update both in the same change set.
