/**
 * @file trace_ring.h
 * @brief Singleton single-producer, single-consumer trace packet ring.
 *
 * The ring owns its storage internally and exposes a packet-oriented API for one producer and one
 * consumer running on separate cores.
 */

#ifndef TRACE_RING_H
#define TRACE_RING_H

#include <stdbool.h>
#include <stdint.h>

#include "config/ring_config.h"
#include "trace/trace_packet.h"

/**
 * @brief Initialize the singleton trace ring.
 *
 * Call this before the producer core starts using the ring.
 */

void trace_ring_init(void);

/**
 * @brief Return the number of queued packets.
 *
 * This is an advisory snapshot only. Do not use it as a pre-check before another cross-core ring
 * operation.
 */
uint32_t trace_ring_available(void);

/**
 * @brief Return the number of free packet slots.
 *
 * This is an advisory snapshot only. Do not use it as a pre-check before another cross-core ring
 * operation.
 */
uint32_t trace_ring_free(void);

/** @brief Return the total number of packets accepted by the producer. */
uint32_t trace_ring_total_produced(void);

/** @brief Return the total number of packets consumed from the ring. */
uint32_t trace_ring_total_consumed(void);

/** @brief Return the number of packets dropped because the ring was full. */
uint32_t trace_ring_dropped_packets(void);

/** @brief Return the maximum observed queued-packet depth. */
uint32_t trace_ring_high_watermark(void);

/**
 * @brief Copy one packet into the ring.
 * @param packet Source packet owned by the caller.
 * @return `true` when the packet was enqueued, otherwise `false` when the ring was full.
 *
 * The packet contents are copied into internal ring storage before this function returns.
 */
bool trace_ring_push(const trace_packet_t *packet);

/**
 * @brief Copy out and consume the oldest queued packet.
 * @param packet_out Caller-owned destination for the copied packet.
 * @return `true` when a packet was copied out, otherwise `false` when the ring was empty.
 *
 * This also invalidates any pointer previously returned by @ref trace_ring_peek.
 */
bool trace_ring_pop_copy(trace_packet_t *packet_out);

/**
 * @brief Borrow the oldest queued packet without consuming it.
 * @return Pointer to ring-owned packet storage, or `NULL` when the ring is empty.
 *
 * The returned pointer remains valid only until the next consume operation on the ring, including
 * @ref trace_ring_pop and @ref trace_ring_pop_copy.
 */
const trace_packet_t *trace_ring_peek(void);

/**
 * @brief Consume the packet currently returned by @ref trace_ring_peek.
 *
 * Call this only after the consumer is fully done with the borrowed packet pointer.
 */
void trace_ring_pop(void);

#endif