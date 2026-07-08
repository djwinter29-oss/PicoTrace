#ifndef TRACE_RING_ATOMIC_H
#define TRACE_RING_ATOMIC_H

#include <stdint.h>

static inline uint32_t trace_ring_load_acquire(volatile uint32_t const *value) {
    return *value;
}

static inline uint32_t trace_ring_load_relaxed(volatile uint32_t const *value) {
    return *value;
}

static inline void trace_ring_store_release(volatile uint32_t *value, uint32_t new_value) {
    *value = new_value;
}

static inline void trace_ring_store_relaxed(volatile uint32_t *value, uint32_t new_value) {
    *value = new_value;
}

static inline void trace_ring_increment_relaxed(volatile uint32_t *value) {
    *value += 1u;
}

#endif