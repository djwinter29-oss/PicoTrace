/**
 * @file ring_config.h
 * @brief User-tunable sizing knobs for the shared trace packet ring.
 */

#ifndef PICO_TRACE_RING_CONFIG_H
#define PICO_TRACE_RING_CONFIG_H

/** @brief Total bytes reserved for one logical trace packet, including the fixed header. */
#ifndef TRACE_PACKET_BYTES
#define TRACE_PACKET_BYTES 256u
#endif

/** @brief Number of fixed packet slots in the singleton trace ring. */
#ifndef TRACE_RING_CAPACITY
#define TRACE_RING_CAPACITY 64u
#endif

_Static_assert(TRACE_RING_CAPACITY > 0u, "TRACE_RING_CAPACITY must be greater than zero");

#endif