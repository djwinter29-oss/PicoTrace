#ifndef PICO_TRACE_RING_CONFIG_H
#define PICO_TRACE_RING_CONFIG_H

/* User-tunable ring buffer sizing knobs for trace capture. */

#ifndef TRACE_PACKET_BYTES
#define TRACE_PACKET_BYTES 128u
#endif

#ifndef TRACE_RING_CAPACITY
#define TRACE_RING_CAPACITY 32u
#endif

_Static_assert(TRACE_RING_CAPACITY > 0u, "TRACE_RING_CAPACITY must be greater than zero");

#endif