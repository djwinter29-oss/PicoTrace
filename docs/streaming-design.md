# Streaming Design

PicoTrace keeps USB ownership simple so the same transport model can support both passive SPI and
passive I2C capture.

## Recommended ownership

- One core owns all TinyUSB interaction.
- A second core, if used, owns sampling, packet preparation, or buffer filling.
- Data crosses the boundary through a small ring buffer or double buffer.

## Why this shape is preferred

CDC, HID, and vendor streaming may look like separate features, but they still share the same USB
device stack and the same USB bus. Splitting those protocol handlers across cores does not remove
bus contention and usually makes timing and shared-state bugs more likely.

Keeping USB on one core gives a clearer ownership model:

- one core runs `tud_task()` and services endpoints
- one core produces data
- one buffer boundary connects them

## Poll order

When streaming is the priority, the preferred order on the USB-owning core is:

1. `tud_task()`
2. vendor stream transmit
3. bounded CDC work
4. bounded HID work

This keeps low-rate control traffic from stealing too much time from streaming.

## CDC and HID expectations

- CDC should stay minimal and low-rate when streaming matters, and it should remain the shared host-control path across supported protocols.
- HID should stay small and deterministic.
- If a derived project does not need one of those interfaces, remove or minimize it rather than letting placeholder behavior grow.

## Upgrade path

The current stream path is now a direct buffer-backed producer/consumer path. PicoTrace does not
emit placeholder vendor traffic when no trace packets are queued.

The current firmware now keeps the consumer side directly in `firmware/src/usb/usb_bulk.c`:

- `usb_bulk_poll_stream()` drains completed `trace_packet_t` records from the trace ring
- partial USB writes retain the peeked ring slot until the full logical packet has been sent
- disabling stream discards any partially transmitted logical packet so the next enable restarts from a clean packet boundary
- no vendor data is emitted when the ring is empty

PicoTrace intentionally keeps the USB-side contract simple:

- the supported software pause boundary is `stream off`, not arbitrary transport suspend/resume
- physical USB disconnect is treated as a likely power-loss or reset event for the RP2040 board, not as a stateful pause that must preserve an in-flight logical packet across reconnection
- malformed queued packets are dropped defensively by the USB consumer instead of trying to repair them in place

That partial-write behavior is an intentional design constraint on the producer side as well:

- the producer must treat already-queued packets as immutable
- queue overflow is handled by dropping the newly produced packet fragment
- the producer reports that loss with overflow metadata instead of overwriting older queued packets

The current I2C monitor scaffold now establishes the capture-side buffer boundary for that upgrade:

- one PIO state machine per I2C channel
- one DMA channel per sampler
- ping-pong word buffers per channel
- DMA IRQ decode and packetization on the producer core before queueing into the trace ring

## Packet-oriented trace queues

For SPI and I2C trace workloads, prefer a fixed-slot packet ring over a generic message queue.

- Keep one core as the USB owner and let the other core assemble complete trace packets.
- Use a single-producer, single-consumer ring of fixed packet slots.
- Keep the common packet size aligned to the USB bulk packet size by choosing a total logical packet size that is a multiple of 64 bytes.

The current trace queue implementation lives under `firmware/src/trace/` and uses a 128-byte logical
packet made of a fixed header plus payload.

For the implementation-level design of that queue, see `docs/details/trace-ring-buffer-design.md`.
For the producer-side PIO sampler and DMA ping-pong buffer boundary that now feeds raw sample
fragments into that queue, see `docs/details/i2c-pio-sampler-design.md`.

The user-tunable sizing knobs live under `firmware/src/config/ring_config.h` so projects can adjust
packet size and ring depth without frequently changing the trace implementation itself.

The queue algorithm and the atomic primitives are intentionally split:

- `firmware/src/trace/trace_ring.c` contains the ring behavior.
- `firmware/src/trace/trace_ring_atomic.h` contains the firmware-side acquire, release, and relaxed helpers.

Host tests may override the atomic helper header with a test-local version. That keeps test-only
behavior out of the main ring implementation while still allowing the ring logic to be exercised on
the host.

Recommended packet boundaries:

- I2C: one packet per transaction when possible, with continuation packets only when the payload limit is exceeded.
- SPI: close a packet when chip select deasserts or when the packet payload limit is reached, whichever comes first.

Do not equate one logical trace packet with one USB packet. One trace packet may span multiple 64-byte
USB transfers.

## Trace ring contract

The trace ring is a single-producer, single-consumer queue.

- One core exclusively pushes complete packets into the ring.
- One core exclusively peeks and pops packets from the ring.
- The ring must be initialized before the producer core starts running.
- The queue storage is a single file-local static instance inside `firmware/src/trace/trace_ring.c`.
- Callers do not allocate or pass a ring handle; the API operates on that singleton queue.
- Dropped-packet status is reported through `trace_ring_dropped_packets()`.
- Total produced and consumed packet counts are reported through `trace_ring_total_produced()` and `trace_ring_total_consumed()`.
- Maximum observed occupancy is reported through `trace_ring_high_watermark()`.
- `trace_ring_push()` copies the caller packet into the ring before returning.
- `trace_ring_pop_copy()` copies the oldest queued packet into caller-owned storage and advances the ring in one step.

The current PicoTrace policy on queue overflow is drop-newest, not overwrite-oldest.

This is intentional because the USB consumer may hold a borrowed pointer returned by
`trace_ring_peek()` across multiple bulk transfers while finishing one logical packet. Overwriting
older queued slots would therefore risk corrupting a packet still being transmitted.

The queue exposes packet availability with `trace_ring_available()` and free space with
`trace_ring_free()`. These are advisory snapshots for status and diagnostics only. They must not be
used as a separate pre-check before `push()` or `peek()` on another core.

The produced, consumed, dropped, and high-watermark counters are intended for instrumentation and
performance tuning. They should not be used as hot-path flow-control decisions.

## Correct consumer usage

`trace_ring_peek()` returns a pointer to the current readable slot inside the ring. That pointer stays
valid only until the next consume operation advances the ring.

When the consumer wants owned storage instead of a borrowed slot pointer, prefer
`trace_ring_pop_copy()`. It avoids pointer-lifetime mistakes at the cost of one packet copy.

Safe zero-copy usage pattern:

1. Call `trace_ring_peek()`.
2. Consume the pointed packet completely.
3. Call `trace_ring_pop()` only after the consumer is done with that packet.

Safe copy-out usage pattern:

1. Declare a local `trace_packet_t` destination.
2. Call `trace_ring_pop_copy(&packet)`.
3. Use the copied packet from caller-owned storage.

Unsafe usage pattern:

1. Call `trace_ring_peek()`.
2. Call `trace_ring_pop()` or `trace_ring_pop_copy()`.
3. Continue using the old pointer.

That last pattern is a race, because either consume path releases the slot back to the producer and
the producer may reuse it immediately.

This matters for USB draining as well. If the USB side can only transmit part of a packet in one pass,
the consumer should keep the peeked pointer plus a local transmit offset and only call `pop()` after
the full logical packet has been sent.

When the software stream gate is turned off, the current PicoTrace policy is to abandon that borrowed
partial-send state and restart from the beginning of the oldest queued logical packet if streaming is
enabled again later. This keeps the implementation simple and preserves explicit packet boundaries.
PicoTrace does not currently try to resume a logical packet from the middle after a software stop.