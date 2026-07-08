# Trace Ring Buffer Design

## Purpose

This document describes the detailed design of the firmware trace ring buffer implemented under
`firmware/src/trace/`.

It focuses on the current PicoTrace implementation, not on a hypothetical future queue. The goal is
to explain the data structures, ownership rules, memory-ordering model, and API contract that make
the current single-producer, single-consumer trace path safe and predictable.

## Related Architecture

For the higher-level component view around this design, see:

- [../architecture/firmware-architecture.md](../architecture/firmware-architecture.md)
- [../architecture/interface-and-synchronization.md](../architecture/interface-and-synchronization.md)

## Design Scope

This design covers:

- `firmware/src/trace/trace_packet.h`
- `firmware/src/trace/trace_ring.h`
- `firmware/src/trace/trace_ring.c`
- `firmware/src/trace/trace_ring_atomic.h`
- `firmware/src/config/ring_config.h`

It also references the current host-side behavioral checks in
`firmware/tests/usb_app_test.c`.

This design does not define protocol-specific SPI or I2C sampling logic. It defines only the packet
container and the queue used to hand complete trace packets from a producer context to a consumer
context.

## Design Goals

- support one producer and one consumer without dynamic allocation
- keep the queue deterministic and bounded
- keep packet storage fixed-size and USB-stream friendly
- let the producer hand off complete packets with one copy into ring-owned storage
- let the consumer choose either zero-copy borrow or copy-out consumption
- keep cross-core synchronization small and explicit
- expose instrumentation counters for bring-up and tuning

## Non-Goals

- multiple producers
- multiple consumers
- variable-size queue nodes
- blocking waits, semaphores, or OS-managed synchronization
- ownership transfer of caller-allocated buffers
- protocol decode policy above the packet layer

## File Roles

### `trace_packet.h`

Defines the fixed packet format shared by all trace producers and consumers.

### `trace_ring.h`

Defines the public singleton queue API and the external contract seen by firmware code.

### `trace_ring.c`

Owns the queue storage and implements the SPSC ring behavior.

### `trace_ring_atomic.h`

Provides the acquire, release, and relaxed atomic helpers used by the ring implementation.

### `ring_config.h`

Defines the user-tunable compile-time sizing knobs:

- `TRACE_PACKET_BYTES`
- `TRACE_RING_CAPACITY`

## Packet Model

Each logical queue element is one fixed-size `trace_packet_t`.

The packet is split into:

- `trace_packet_header_t header`
- `uint8_t payload[TRACE_PACKET_PAYLOAD_BYTES]`

The header fields are:

- `version`
- `type`
- `channel`
- `flags`
- `payload_len`
- `meta`
- `sequence`
- `timestamp_us`

The queue treats packets as opaque fixed-size blobs. It does not inspect protocol fields other than
copying the whole packet in and out.

### Packet Size

`TRACE_PACKET_PAYLOAD_BYTES` is derived from:

`TRACE_PACKET_BYTES - sizeof(trace_packet_header_t)`

With the current defaults in `ring_config.h`:

- `TRACE_PACKET_BYTES = 128`
- `TRACE_RING_CAPACITY = 32`

That gives a default queue payload budget of 32 fixed packet slots, each 128 bytes wide.

At the default settings, the ring reserves 4096 bytes for packet storage alone:

$$32 \times 128 = 4096\ \text{bytes}$$

This is deliberate:

- 128 bytes is a simple multiple of the 64-byte USB bulk packet size
- fixed-size slots keep indexing and ownership simple
- one logical trace packet may still span multiple USB transfers

## Queue Topology

The implementation uses one file-local singleton instance:

- `static trace_ring_t g_trace_ring;`

The internal state is:

- `slots[TRACE_RING_CAPACITY]`
- `write_index`
- `read_index`
- `total_produced_packets`
- `total_consumed_packets`
- `dropped_packets`
- `high_watermark_packets`

There is no externally allocated ring handle. All callers operate on the singleton queue.

## Why Singleton Ownership Was Chosen

The current firmware model only needs one capture handoff path between one producer side and one
consumer side. A singleton queue avoids:

- handle plumbing through unrelated modules
- duplicate initialization paths
- accidental multiple rings with diverging behavior

This is the smallest design that supports the current architecture.

## Indexing Model

The queue uses monotonically increasing 32-bit `write_index` and `read_index` counters.

- the producer advances `write_index`
- the consumer advances `read_index`
- queue occupancy is `write_index - read_index`
- slot selection is `index % TRACE_RING_CAPACITY`

This means the ring does not reset indices when they reach capacity. Instead, it uses modular slot
mapping while the logical indices continue to increase.

### Wraparound Behavior

The design relies on unsigned 32-bit arithmetic.

That is safe here because the producer-consumer distance is intentionally bounded by
`TRACE_RING_CAPACITY`. The queue never allows the producer to advance more than one full ring ahead
of the consumer, so the subtraction-based occupancy check remains valid across normal 32-bit index
wraparound.

The packet counters such as `total_produced_packets` and `total_consumed_packets` also use 32-bit
unsigned arithmetic and will eventually wrap after long enough runtime. They are intended for
instrumentation, not as permanent monotonic lifetime counters.

## Storage Model

Each push copies a caller-owned `trace_packet_t` into one internal slot.

This choice keeps the queue contract simple:

- the caller does not need to keep the source packet alive after `trace_ring_push()` returns
- the queue always owns the readable storage seen by the consumer
- the consumer can borrow or copy from stable ring-owned memory

The cost is one packet copy per successful enqueue. For the current design, that is acceptable in
exchange for clear ownership and bounded memory.

## Concurrency Model

The queue is strictly single-producer, single-consumer.

Required ownership rules:

- only one execution context may call `trace_ring_push()`
- only one execution context may call `trace_ring_peek()`, `trace_ring_pop()`, or `trace_ring_pop_copy()`
- `trace_ring_init()` must run before the producer starts using the queue

The implementation is designed for separate firmware contexts, including separate cores, but it does
not require an RTOS.

## Atomic And Memory-Ordering Model

The queue uses small helper wrappers around GCC-style atomic builtins:

- acquire loads
- release stores
- relaxed loads and stores
- relaxed increments

The design intent is:

- the producer fully writes packet contents before publishing a new `write_index`
- the consumer observes `write_index` with acquire ordering before reading a published slot
- the consumer fully finishes consuming a slot before publishing the new `read_index`
- the producer observes `read_index` with acquire ordering before deciding whether space is available

### Producer Publication Sequence

Producer-side `trace_ring_push()` does the following:

1. read the current `write_index` with relaxed ordering
2. read the current `read_index` with acquire ordering
3. reject the push if the queue is full
4. copy the packet into `slots[write_index % TRACE_RING_CAPACITY]`
5. update producer-side counters
6. publish the new `write_index` with release ordering

The release store on `write_index` is the publication point for the slot contents.

### Consumer Acquisition Sequence

Consumer-side `trace_ring_peek()` and `trace_ring_pop_copy()` do the following:

1. read the current `read_index` with relaxed ordering
2. read the current `write_index` with acquire ordering
3. if equal, treat the queue as empty
4. otherwise read the slot at `read_index % TRACE_RING_CAPACITY`

The acquire load on `write_index` ensures the consumer does not read stale packet contents after the
producer has published a slot.

### Consumer Release Sequence

When the consumer is done with a slot, `trace_ring_pop()` or `trace_ring_pop_copy()` advances
`read_index` with a release store.

That release store is the point where the slot becomes reusable by the producer.

### Why Counters Use Relaxed Ordering

Instrumentation counters such as produced, consumed, dropped, and high-watermark values do not
control slot ownership. They are not used to make correctness-critical decisions about which slot is
readable or writable.

Because of that, relaxed atomic increments and stores are sufficient for those counters.

## Full Condition

The queue is full when:

`write_index - read_index >= TRACE_RING_CAPACITY`

When full:

- `trace_ring_push()` returns `false`
- the packet is not copied into the ring
- `dropped_packets` is incremented

This is a drop-newest overflow policy. The oldest queued packet remains untouched.

The current design drops on overflow rather than blocking the producer.

This is appropriate for a passive trace path where bounded runtime behavior matters more than forcing
the producer to wait indefinitely.

The design also intentionally does not overwrite the oldest queued slot on overflow. In the current
PicoTrace consumer path, the USB side may keep a borrowed pointer returned by `trace_ring_peek()`
while it completes a multi-transfer write of that logical packet. Overwriting the oldest slot would
therefore create a producer-consumer race against in-flight USB transmission.

## Empty Condition

The queue is empty when:

`write_index == read_index`

When empty:

- `trace_ring_peek()` returns `NULL`
- `trace_ring_pop_copy()` returns `false`
- `trace_ring_pop()` returns immediately without side effects

## Public API Contract

### `trace_ring_init()`

Zeroes the singleton state, including slots, indices, and counters.

This must run before cross-core use starts.

### `trace_ring_push(const trace_packet_t *packet)`

- copies one complete caller-owned packet into the ring
- returns `true` on success
- returns `false` when the ring is full
- asserts that `packet != NULL`

### `trace_ring_peek()`

- returns a borrowed pointer to the oldest queued packet
- returns `NULL` when empty
- does not consume the packet

The returned pointer remains valid only until the next consume operation on the queue.

### `trace_ring_pop()`

- consumes the packet currently at the read position
- is intended to pair with `trace_ring_peek()`
- does nothing when the queue is already empty

### `trace_ring_pop_copy(trace_packet_t *packet_out)`

- copies the oldest queued packet into caller-owned storage
- consumes that packet in one operation
- returns `true` on success
- returns `false` when empty
- asserts that `packet_out != NULL`

### Advisory Snapshot Functions

The following are explicitly advisory snapshots:

- `trace_ring_available()`
- `trace_ring_free()`

They are safe for diagnostics and status reporting but must not be used as a separate readiness test
before another cross-context queue action. The queue state may change immediately after the snapshot.

### Instrumentation Functions

The following expose counters intended for diagnostics and tuning:

- `trace_ring_total_produced()`
- `trace_ring_total_consumed()`
- `trace_ring_dropped_packets()`
- `trace_ring_high_watermark()`

These counters are useful for:

- confirming producer activity
- confirming consumer progress
- sizing the queue depth
- detecting sustained overflow pressure

They are not intended as hot-path flow-control inputs.

## Consumer Usage Patterns

### Zero-Copy Borrow Pattern

Use `trace_ring_peek()` when the consumer can finish with the packet while it is still borrowed from
ring-owned storage.

Safe sequence:

1. call `trace_ring_peek()`
2. read the packet contents completely
3. call `trace_ring_pop()` only after the consumer is finished with the borrowed pointer

This pattern is appropriate when the consumer needs to avoid an extra copy, such as when draining a
packet to USB in place.

### Copy-Out Pattern

Use `trace_ring_pop_copy()` when the consumer wants owned storage.

Safe sequence:

1. allocate or declare a local `trace_packet_t`
2. call `trace_ring_pop_copy(&packet)`
3. continue using the copied packet independently of queue state

This pattern is simpler when the consumer logic spans multiple steps and does not want borrowed
pointer lifetime constraints.

### Unsafe Pattern

The following sequence is invalid:

1. call `trace_ring_peek()`
2. call `trace_ring_pop()` or `trace_ring_pop_copy()`
3. continue using the old pointer

After the consume step, the producer is allowed to reuse that slot.

## High-Watermark Behavior

`high_watermark_packets` records the maximum observed queue occupancy.

It is updated only on successful push. The occupancy used for the comparison is:

`(write_index - read_index) + 1`

That means the high-watermark reflects the queued-packet depth after the new packet is accepted.

## Default Memory Footprint

At the current defaults:

- packet storage: `32 * 128 = 4096` bytes
- plus queue indices and counters

The queue is intentionally small enough for low-cost RP2040-class firmware while still buffering
multiple transactions or transaction fragments between the producer and the USB consumer.

## Tuning Knobs

### `TRACE_PACKET_BYTES`

Increase this when:

- per-transaction metadata or payload regularly exceeds the current packet payload budget
- reducing continuation packets is more valuable than preserving the current RAM footprint

Tradeoff:

- larger slots reduce fragmentation pressure but increase per-slot copy cost and static RAM use

### `TRACE_RING_CAPACITY`

Increase this when:

- temporary consumer stalls are expected
- USB draining is bursty relative to trace production
- dropped-packet counts indicate the queue is too shallow

Tradeoff:

- deeper rings improve burst absorption but increase static RAM use linearly

## Test-Backed Behavioral Guarantees

The current host tests in `firmware/tests/usb_app_test.c` verify at least these behaviors:

- a pushed packet can be peeked and popped correctly
- `trace_ring_pop_copy()` returns an owned copy of the oldest packet
- `trace_ring_pop_copy()` advances past a previously peeked packet
- the ring reports full at capacity and increments the drop counter on overflow
- produced, consumed, available, free, and high-watermark counters track the expected state changes

These tests do not prove multicore timing behavior exhaustively, but they do lock down the public
queue contract and the expected state transitions.

## Current Limitations

- singleton queue only
- SPSC only
- packet copy required on enqueue
- no blocking or wakeup mechanism
- no explicit counter saturation handling beyond natural 32-bit wraparound
- no built-in partial-packet reassembly policy at the queue layer

## Why This Design Fits PicoTrace

The queue matches the current PicoTrace architecture because it gives the producer and USB consumer a
small, deterministic handoff boundary:

- the producer assembles complete protocol packets
- the ring preserves packet boundaries
- the consumer drains those packets toward USB
- the queue exposes enough counters to tune loss versus memory use

That is the right level of mechanism for a low-cost passive trace tool. It keeps the cross-core
boundary explicit without pushing protocol policy down into the queue implementation.