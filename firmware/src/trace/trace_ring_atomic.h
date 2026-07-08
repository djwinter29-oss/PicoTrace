/**
 * @file trace_ring_atomic.h
 * @brief Small atomic helpers used by the trace ring implementation.
 *
 * These helpers isolate the acquire, release, and relaxed operations used by the SPSC ring so the
 * ring algorithm can stay free of compiler-specific branches.
 */

#ifndef TRACE_RING_ATOMIC_H
#define TRACE_RING_ATOMIC_H

#include <stdint.h>

/** @brief Load a 32-bit value with acquire ordering. */
static inline uint32_t trace_ring_load_acquire(volatile uint32_t const *value) {
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

/** @brief Load a 32-bit value with relaxed ordering. */
static inline uint32_t trace_ring_load_relaxed(volatile uint32_t const *value) {
    return __atomic_load_n(value, __ATOMIC_RELAXED);
}

/** @brief Store a 32-bit value with release ordering. */
static inline void trace_ring_store_release(volatile uint32_t *value, uint32_t new_value) {
    __atomic_store_n(value, new_value, __ATOMIC_RELEASE);
}

/** @brief Store a 32-bit value with relaxed ordering. */
static inline void trace_ring_store_relaxed(volatile uint32_t *value, uint32_t new_value) {
    __atomic_store_n(value, new_value, __ATOMIC_RELAXED);
}

/** @brief Increment a 32-bit value with relaxed ordering. */
static inline void trace_ring_increment_relaxed(volatile uint32_t *value) {
    __atomic_add_fetch(value, 1u, __ATOMIC_RELAXED);
}

#endif