# I2C PIO Sampler Design

## Purpose

This document describes the current firmware scaffold for passive I2C sampling, oversampled I2C
decode, and trace packetization.

The focus here is the capture boundary from PIO sampling into per-channel ping-pong DMA buffers,
then into a per-channel decoder state and a persistent per-channel trace packet buffer.

## Related Architecture

For the higher-level component view around this design, see:

- [../architecture/firmware-architecture.md](../architecture/firmware-architecture.md)
- [../architecture/interface-and-synchronization.md](../architecture/interface-and-synchronization.md)

## Scope

This design covers:

- `firmware/src/trace/i2c/i2c_monitor.pio`
- `firmware/src/trace/i2c/i2c_monitor.c`
- `firmware/src/trace/i2c/i2c_monitor.h`
- `firmware/src/trace/i2c/i2c_decoder.c`
- `firmware/src/trace/i2c/i2c_decoder.h`
- `firmware/src/trace/i2c/i2c_trace_packet.c`
- `firmware/src/trace/i2c/i2c_trace_packet.h`
- `firmware/src/config/i2c_monitor_config.h`

## Current Design Choice

The current scaffold uses four independent samplers instead of one shared 8-pin sampler.

Reasons:

- cross-channel sample synchronization is not required
- each I2C channel can run at its own configured sample rate
- the existing hardware allocation does not need to be remapped into one contiguous 8-pin window
- the later software decoder can stay per-channel instead of demultiplexing one shared raw stream

Each I2C channel gets:

- one PIO state machine
- one two-pin sampling program
- one DMA channel
- two ping-pong raw buffers

The sampler now starts idle at boot. A channel begins sampling only when the producer-side control
path requests a non-zero oversampling rate for that channel. Passing `0` stops that channel and
resets its decoder, packet builder, DMA state, and pins back to plain input GPIO state.

Changing a channel from one non-zero sample rate to another is implemented the same way: the
current channel instance is stopped first, then a fresh channel instance is started with the new
rate. This is intentionally destructive. Any in-flight raw samples, partial decode state, open
packet-builder state, completed-buffer counters, and sticky overrun state for that running session
are discarded as part of the retune.

## Hardware Mapping Assumption

The default channel mapping follows `docs/hardware-connections.md`:

- channel `0`: `GPIO16` and `GPIO17`
- channel `1`: `GPIO18` and `GPIO19`
- channel `2`: `GPIO20` and `GPIO21`
- channel `3`: `GPIO26` and `GPIO27`

Each channel must use an adjacent `SDA` and `SCL` pair, because the current PIO program samples two
consecutive pins with one `in pins, 2` instruction.

## PIO Program

The current PIO sampler is intentionally minimal:

```pio
.wrap_target
    in pins, 2
.wrap
```

At the configured sampling rate, each instruction samples one two-pin `SDA` and `SCL` pair and
shifts the result into the input shift register.

Autopush is configured at 32 bits. Because each sample contributes 2 bits, one FIFO word contains:

$$32 / 2 = 16\ \text{samples per 32-bit word}$$

This is the key packing choice in the current scaffold. It is denser than storing one byte per
sample and is a better fit for per-channel DMA buffers.

## Buffer Layout

Each channel owns two raw buffers:

- buffer `0`
- buffer `1`

Each buffer contains `I2C_MONITOR_BUFFER_WORDS` 32-bit words.

At the current default:

- `I2C_MONITOR_BUFFER_WORDS = 64`

That gives:

$$64 \times 4 = 256\ \text{bytes per buffer}$$

Per channel, the ping-pong pair consumes:

$$2 \times 256 = 512\ \text{bytes}$$

Across four channels, the raw sample buffers consume:

$$4 \times 512 = 2048\ \text{bytes}$$

## DMA Ownership Model

Each channel uses one DMA channel.

The DMA configuration is:

- 32-bit transfers
- read from the PIO RX FIFO
- no read increment
- write increment enabled
- DREQ paced by the owning PIO state machine

The DMA writes into one active ping-pong buffer until `I2C_MONITOR_BUFFER_WORDS` words are filled.

When that transfer completes:

1. the DMA interrupt marks the completed buffer as ready
2. the implementation advances to the other buffer
3. the DMA channel is restarted against the alternate buffer

This makes the completed buffer stable for software consumption while the alternate buffer begins
collecting the next raw sample block.

## Buffer Completion Path

The current implementation handles completed DMA buffers directly in the DMA IRQ path.

For each completed buffer the IRQ path:

- acknowledges the DMA completion
- identifies the just-finished ping-pong slot
- immediately re-arms DMA on the alternate slot
- marks the completed ping-pong slot as software-owned and ready for decode

Then the producer-core poll path:

- stops any active channel whenever streaming is disabled
- passes one completed ping-pong slot at a time to the decoder
- appends decoded I2C items into a persistent per-channel trace packet buffer

This keeps DMA completion capture-first while moving decode and packet assembly out of the hard IRQ
path. Packet assembly is still no longer forced to flush at DMA buffer boundaries.

The current implementation does not keep a third staging copy. A completed DMA slot remains
software-owned until decode finishes, and DMA is only re-armed onto the alternate slot if that slot
is no longer software-owned. If the alternate slot is still owned by software when a completion
arrives, the channel takes the existing stop-first overflow recovery path.

## Current Buffer Handoff Contract

The current implementation no longer uses an external callback hook and no longer treats one DMA
buffer as one trace-packet unit.

Instead, each I2C channel owns:

- one persistent decoder state object
- one persistent open `trace_packet_t` assembly buffer

For each completed DMA buffer:

- `trace/i2c/i2c_decoder.c` walks the oversampled `SDA` and `SCL` stream
- decoded items are reported back to `i2c_monitor.c` as fixed two-byte event/value pairs
- `trace/i2c/i2c_trace_packet.c` appends those pairs into the channel's open trace packet
- if the packet fills before the I2C transaction ends, it is pushed immediately and a continuation
    packet is opened for the same transaction on the next emitted item
- if a `STOP` event is decoded, the current packet is pushed as the end of that I2C transaction
- otherwise the partial packet remains open for the next completed DMA buffer

That means the current implementation proves:

- PIO sampling works per channel
- DMA fills alternating ping-pong buffers
- oversampled ping-pong buffers are decoded into I2C events
- partial packet state survives across DMA IRQs until a transaction ends or a packet fills
- `usb_bulk_poll_stream()` can drain those decoded packets onto the vendor bulk interface when they are available

It still does not prove:

- final compact transaction framing for host consumption
- full error classification for malformed or ambiguous bus activity

If the monitor needs to cut a channel at a boundary, the current implementation now uses one
shared stop-first transition route for all of these cases:

- `OVERFLOW`
- `CONTROL_RECONFIG`
- `CONTROL_STOP`
- `ERROR`

That route stops channel hardware first, emits the boundary event against the existing packet
builder state, clears staged and decode state, and then either leaves the channel stopped or
restarts it with the requested sample rate. If the boundary event cannot be queued and the channel
is restarted, the next successful packet is marked with overflow instead.

The current event stream also uses explicit boundary events for known non-bus causes:

- `OVERFLOW` when capture continuity is lost because buffering could not keep up
- `CONTROL_RECONFIG` when monitoring is intentionally restarted because the sample rate changed
- `CONTROL_STOP` when monitoring is intentionally stopped by control action

`ERROR` is reserved as the default fallback when the monitor needs to report an error boundary but
does not have a more specific event type. In the current design, known boundaries are classified as
`OVERFLOW`, `CONTROL_RECONFIG`, or `CONTROL_STOP`, so normal operation may not emit a generic
`ERROR` event at all.

## Sample Rate Model

Each channel has an independent runtime `sample_hz` selected when that channel is started.

The recommended default for a caller that wants a simple starting point is:

- `I2C_MONITOR_DEFAULT_SAMPLE_HZ = 8000000`

This remains a comfortable starting point for 400 kHz passive observation while still keeping the
scaffold simple. Because channels are independent, control code may choose different oversampling
rates per channel or stop unused channels entirely by passing `0`.

## Overrun Handling

The current scaffold tracks packet-emission failures per channel.

If the decoder or packet appender cannot push a completed packet fragment into the trace ring, the
current transaction fragment is dropped, a sticky channel overrun flag is set, and the channel
overrun count is incremented.

This remains a capture-first choice: preserving continuity of the DMA sample pipeline is treated as
more important than preserving every decoded fragment when the ring cannot accept output.

The current implementation does not overwrite already-queued ring packets on overrun. That would
conflict with the current zero-copy USB consumer, which may hold a borrowed pointer into the oldest
ring slot across partial USB writes. In other words, the current producer policy is explicitly
drop-newest-on-overflow, not overwrite-oldest.

When a completed fragment cannot be queued:

- the fragment that just failed to enqueue is discarded
- the channel sticky overrun state remains set until the channel is stopped or restarted
- the next successfully emitted trace packet is marked so the host can detect that loss occurred

When a second completed raw DMA block arrives before the producer-core poll path has consumed the
already staged block for that channel, the current design treats that as an overrun boundary. The
DMA IRQ immediately stops that channel hardware, latches an `OVERFLOW` transition with the saved
restart rate, and does no packetization or ring pushes itself. The producer-core poll path then
finishes the same stop-first transition route: it emits `OVERFLOW`, clears state, and restarts the
channel with the saved sample rate so later decode resumes from a clean boundary. If streaming has
been disabled before that replay happens, the pending transition is collapsed to stop-only and the
channel is not restarted. If the boundary event cannot be emitted, the next successfully emitted
trace packet from that restarted channel is marked with overflow status instead.

If stream disable races with a latched `OVERFLOW` transition, the current implementation preserves
that `OVERFLOW` boundary as the loss reason and then keeps the channel stopped. It does not rewrite
that pending overflow into `CONTROL_STOP`.

On the USB consumer side, PicoTrace keeps the complementary simplification: `stream off` is the
supported pause path and resets any partial bulk-packet transmit progress back to a packet boundary.
The design does not attempt to preserve mid-packet USB transmit state across a later `stream on`.
Physical USB disconnect is likewise treated as a likely board power-loss or reset event rather than
as a reconnect scenario that must preserve an in-flight logical trace packet.

Cross-core control requests do not execute pending monitor transitions. If a channel already has a
latched next transition, `set rate` fails fast so the caller can retry later instead of mutating
recovery state on behalf of the producer core.

`get status` remains available during those producer-owned transitions. It returns the last current
channel snapshot together with `transition_pending` and `transition_reason` so host-side tools can
observe that the channel is between stable states without having the read itself advance recovery.

Cross-core `set rate` requests also fail fast when streaming is disabled and the requested rate is
non-zero. A caller may still send `0` to stop a channel, but may not start or restart capture while
the shared stream gate is off.

When streaming is disabled, the producer-side poll path routes each running channel through the
same stop-first transition path with `CONTROL_STOP`. That stop disables the channel DMA IRQ, stops
the PIO state machine, emits the stop boundary if it can be queued, and then clears staged raw
buffers, partially filled DMA buffer contents, decoder state, packet-builder state, counters, and
sticky overrun state for that streaming session.

Re-enabling streaming does not restore the old monitor configuration automatically. A caller that
wants capture again must start the desired channels explicitly.

## Why This Stops Before Decode

The correct next stage is:

- read one completed buffer
- decode raw `SDA` and `SCL` samples into per-channel I2C states
- detect `START`, `STOP`, data bits, and `ACK/NACK`
- assemble protocol-level bytes and transactions
- emit fixed trace packets into the trace ring

That decode layer is intentionally not bundled into the PIO and DMA scaffold. Keeping sampling,
decode, and packetization separate makes bring-up easier and keeps failure modes isolated.

## Relationship To The Trace Ring

The ping-pong raw buffers are upstream of the trace ring, but the current implementation decodes
them before queueing and keeps packet assembly state per channel.

Completed raw DMA buffers are decoded into event fragments such as:

- `START`
- `DATA`
- `ACK`
- `STOP`
- `ERROR`
- `OVERFLOW`
- `CONTROL_RECONFIG`
- `CONTROL_STOP`

Normal bus-traffic events stay in the low numeric range, while synthetic boundary and error events
start at `0x80` so host-side decoders can distinguish them cheaply without a second sideband.

Those decoded events are then packed into one or more fixed-size `trace_packet_t` records and
pushed into the singleton queue described in `docs/details/trace-ring-buffer-design.md`.

The packet boundary rule is transaction-oriented rather than DMA-buffer-oriented:

- if a packet fills before a `STOP`, it is pushed as a continued fragment
- if a `STOP` is decoded, the current packet is pushed as the end of the transaction
- otherwise the open packet is retained inside the channel state for the next DMA completion

Within the current I2C packet payload format:

- each decoded item is `2` bytes
- byte `0` is the event type
- byte `1` is the associated value

The current `trace_packet_header_t.meta` field is used as the number of decoded items in that
packet fragment.

These are decoded event fragments, not final transaction summaries. They keep the producer-to-
consumer ownership model real while still leaving room for a later higher-level transaction packer.

## Current Core Ownership

The current firmware now splits ownership across the RP2040 cores:

- core `0` owns `tud_task()`, CDC, HID, and vendor bulk transmit
- core `1` owns capture-side bring-up and the DMA IRQ path that decodes and produces trace packets
- the singleton trace ring under `firmware/src/trace/` remains the ownership boundary between them

The runtime channel control API is also owned by core `1`. If a USB-side command path needs to
start, stop, or retune one monitor channel, it should forward that request onto the producer core
instead of touching the monitor directly from core `0`.

This keeps all TinyUSB interaction on one core while allowing the producer side to run without USB
service work in the same loop.

## Known Limitations Of The Current Scaffold

- one DMA channel per monitored I2C channel, restarted from the DMA IRQ handler
- ring packets currently carry decoded event fragments rather than final transaction summaries
- the DMA IRQ path still does one bounded raw-buffer copy into a staging slot before the producer
    poll path decodes it
- decode timing assumptions are based on oversampled SCL rising-edge reconstruction and have not yet
    been tuned against every malformed or marginal bus waveform case

## Next Step

The next implementation step should be a higher-level transaction packer or host-side interpreter
that consumes decoded `START`, `DATA`, `ACK`, and `STOP` fragments and emits compact I2C transfer
summaries for host tooling.