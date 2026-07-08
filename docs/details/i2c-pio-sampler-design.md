# I2C PIO Sampler Design

## Purpose

This document describes the current firmware scaffold for passive I2C sampling, oversampled I2C
decode, and trace packetization.

The focus here is the capture boundary from PIO sampling into per-channel ping-pong DMA buffers,
then into a per-channel decoder state and a persistent per-channel trace packet buffer.

## Scope

This design covers:

- `firmware/src/trace/capture/i2c_monitor.pio`
- `firmware/src/trace/capture/i2c_monitor.c`
- `firmware/src/trace/capture/i2c_monitor.h`
- `firmware/src/trace/decode/i2c_decoder.c`
- `firmware/src/trace/decode/i2c_decoder.h`
- `firmware/src/trace/decode/i2c_trace_packet.c`
- `firmware/src/trace/decode/i2c_trace_packet.h`
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

For each completed buffer the handler:

- acknowledges the DMA completion
- identifies the just-finished ping-pong slot
- immediately re-arms DMA on the alternate slot
- passes the completed raw sample block to the decoder
- appends decoded I2C items into a persistent per-channel trace packet buffer

This means DMA completion is still handled capture-first, but packet assembly is no longer forced to
flush at DMA buffer boundaries.

## Current Buffer Handoff Contract

The current implementation no longer uses an external callback hook and no longer treats one DMA
buffer as one trace-packet unit.

Instead, each I2C channel owns:

- one persistent decoder state object
- one persistent open `trace_packet_t` assembly buffer

For each completed DMA buffer:

- `trace/decode/i2c_decoder.c` walks the oversampled `SDA` and `SCL` stream
- decoded items are reported back to `i2c_monitor.c` as fixed two-byte event/value pairs
- `trace/decode/i2c_trace_packet.c` appends those pairs into the channel's open trace packet
- if the packet fills before the I2C transaction ends, it is pushed immediately and a continuation
    packet is opened for the same transaction on the next emitted item
- if a `STOP` event is decoded, the current packet is pushed as the end of that I2C transaction
- otherwise the partial packet remains open for the next completed DMA buffer

That means the current implementation proves:

- PIO sampling works per channel
- DMA fills alternating ping-pong buffers
- oversampled ping-pong buffers are decoded into I2C events
- partial packet state survives across DMA IRQs until a transaction ends or a packet fills
- `usb_stream_poll()` can drain those decoded packets before falling back to the placeholder stream

It still does not prove:

- final compact transaction framing for host consumption
- full error classification for malformed or ambiguous bus activity

## Sample Rate Model

Each channel has an independent `sample_hz` in `i2c_monitor_channel_config_t`.

The default is:

- `I2C_MONITOR_DEFAULT_SAMPLE_HZ = 8000000`

This was chosen as a more comfortable starting point for 400 kHz passive observation than 4 MHz,
while still keeping the scaffold simple.

Because channels are independent, setup code may lower the sample rate for known 100 kHz buses or
disable unused channels entirely.

## Overrun Handling

The current scaffold tracks packet-emission failures per channel.

If the decoder or packet appender cannot push a completed packet fragment into the trace ring, the
current transaction fragment is dropped and the channel overrun count is incremented.

This remains a capture-first choice: preserving continuity of the DMA sample pipeline is treated as
more important than preserving every decoded fragment when the ring cannot accept output.

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

## Known Limitations Of The Current Scaffold

- no multicore split yet; the current scaffold is initialized from the existing main loop
- one DMA channel per monitored I2C channel, restarted from the DMA IRQ handler
- ring packets currently carry decoded event fragments rather than final transaction summaries
- decode and packet append work currently execute in the DMA IRQ path
- decode timing assumptions are based on oversampled SCL rising-edge reconstruction and have not yet
    been tuned against every malformed or marginal bus waveform case

## Next Step

The next implementation step should be a higher-level transaction packer or host-side interpreter
that consumes decoded `START`, `DATA`, `ACK`, and `STOP` fragments and emits compact I2C transfer
summaries for host tooling.