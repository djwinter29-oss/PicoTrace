/**
 * @file trace_ring.c
 * @brief Internal storage and implementation for the singleton trace packet ring.
 */

#include "trace/trace_ring.h"
#include "trace/trace_ring_atomic.h"

#include <assert.h>
#include <stddef.h>
#include <string.h>

/**
 * @brief Internal singleton ring state.
 *
 * This structure is private to the implementation and holds both the fixed packet storage and the
 * producer/consumer indices and counters used by the SPSC algorithm.
 */
typedef struct {
    trace_packet_t slots[TRACE_RING_CAPACITY];   /**< Fixed packet slots owned by the ring. */
    volatile uint32_t write_index;               /**< Monotonic producer write position. */
    volatile uint32_t read_index;                /**< Monotonic consumer read position. */
    volatile uint32_t total_produced_packets;    /**< Total packets successfully written into the ring. */
    volatile uint32_t total_consumed_packets;    /**< Total packets consumed from the ring. */
    volatile uint32_t dropped_packets;           /**< Total packets rejected because the ring was full. */
    volatile uint32_t high_watermark_packets;    /**< Maximum observed queued-packet depth. */
} trace_ring_t;

/** @brief Singleton ring state shared by the producer and consumer. */
static trace_ring_t g_trace_ring;

void trace_ring_init(void) {
    trace_ring_t *ring = &g_trace_ring;

    memset(ring, 0, sizeof(*ring));
}

uint32_t trace_ring_available(void) {
    const trace_ring_t *ring = &g_trace_ring;
    uint32_t write_index;
    uint32_t read_index;

    write_index = trace_ring_load_acquire(&ring->write_index);
    read_index = trace_ring_load_acquire(&ring->read_index);
    return write_index - read_index;
}

uint32_t trace_ring_free(void) {
    uint32_t available = trace_ring_available();

    if (available >= TRACE_RING_CAPACITY) {
        return 0u;
    }

    return TRACE_RING_CAPACITY - available;
}

uint32_t trace_ring_dropped_packets(void) {
    const trace_ring_t *ring = &g_trace_ring;

    return trace_ring_load_acquire(&ring->dropped_packets);
}

uint32_t trace_ring_total_produced(void) {
    const trace_ring_t *ring = &g_trace_ring;

    return trace_ring_load_acquire(&ring->total_produced_packets);
}

uint32_t trace_ring_total_consumed(void) {
    const trace_ring_t *ring = &g_trace_ring;

    return trace_ring_load_acquire(&ring->total_consumed_packets);
}

uint32_t trace_ring_high_watermark(void) {
    const trace_ring_t *ring = &g_trace_ring;

    return trace_ring_load_acquire(&ring->high_watermark_packets);
}

bool trace_ring_push(const trace_packet_t *packet) {
    trace_ring_t *ring = &g_trace_ring;
    uint32_t high_watermark;
    uint32_t occupancy;
    uint32_t write_index;
    uint32_t read_index;
    uint32_t slot_index;

    assert(packet != NULL);

    write_index = trace_ring_load_relaxed(&ring->write_index);
    read_index = trace_ring_load_acquire(&ring->read_index);
    if ((write_index - read_index) >= TRACE_RING_CAPACITY) {
        trace_ring_increment_relaxed(&ring->dropped_packets);
        return false;
    }

    slot_index = write_index % TRACE_RING_CAPACITY;
    memcpy(&ring->slots[slot_index], packet, sizeof(*packet));
    trace_ring_increment_relaxed(&ring->total_produced_packets);
    occupancy = (write_index - read_index) + 1u;
    high_watermark = trace_ring_load_relaxed(&ring->high_watermark_packets);
    if (occupancy > high_watermark) {
        trace_ring_store_relaxed(&ring->high_watermark_packets, occupancy);
    }
    trace_ring_store_release(&ring->write_index, write_index + 1u);
    return true;
}

bool trace_ring_pop_copy(trace_packet_t *packet_out) {
    trace_ring_t *ring = &g_trace_ring;
    uint32_t write_index;
    uint32_t read_index;
    uint32_t slot_index;

    assert(packet_out != NULL);

    read_index = trace_ring_load_relaxed(&ring->read_index);
    write_index = trace_ring_load_acquire(&ring->write_index);
    if (write_index == read_index) {
        return false;
    }

    slot_index = read_index % TRACE_RING_CAPACITY;
    memcpy(packet_out, &ring->slots[slot_index], sizeof(*packet_out));
    trace_ring_increment_relaxed(&ring->total_consumed_packets);
    trace_ring_store_release(&ring->read_index, read_index + 1u);
    return true;
}

const trace_packet_t *trace_ring_peek(void) {
    const trace_ring_t *ring = &g_trace_ring;
    uint32_t write_index;
    uint32_t read_index;
    uint32_t slot_index;

    read_index = trace_ring_load_relaxed(&ring->read_index);
    write_index = trace_ring_load_acquire(&ring->write_index);
    if (write_index == read_index) {
        return NULL;
    }

    slot_index = read_index % TRACE_RING_CAPACITY;
    return &ring->slots[slot_index];
}

void trace_ring_pop(void) {
    trace_ring_t *ring = &g_trace_ring;
    uint32_t write_index;
    uint32_t read_index;

    read_index = trace_ring_load_relaxed(&ring->read_index);
    write_index = trace_ring_load_acquire(&ring->write_index);
    if (write_index == read_index) {
        return;
    }

    trace_ring_increment_relaxed(&ring->total_consumed_packets);
    trace_ring_store_release(&ring->read_index, read_index + 1u);
}