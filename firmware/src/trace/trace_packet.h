/**
 * @file trace_packet.h
 * @brief Fixed trace packet layout shared by trace producers and consumers.
 */

#ifndef TRACE_PACKET_H
#define TRACE_PACKET_H

#include <stdint.h>

#include "config/ring_config.h"

/** @brief Trace packet format version stored in each packet header. */
#define TRACE_PACKET_VERSION 1u

/** @brief Logical trace source carried by a packet. */
typedef enum {
    TRACE_TYPE_I2C = 1u, /**< Packet payload carries passive I2C trace data. */
    TRACE_TYPE_SPI = 2u, /**< Packet payload carries passive SPI trace data. */
} trace_type_t;

/** @brief Packet flags describing boundaries, continuation, truncation, or capture errors. */
typedef enum {
    TRACE_FLAG_END = 1u << 0, /**< This fragment closes the current logical transaction or packet. */
    TRACE_FLAG_CONTINUED = 1u << 1, /**< This fragment continues data started by an earlier packet. */
    TRACE_FLAG_OVERFLOW = 1u << 2, /**< Capture continuity was lost before this fragment. */
    TRACE_FLAG_TRUNCATED = 1u << 3, /**< Payload data was shortened relative to the observed input. */
    TRACE_FLAG_ERROR = 1u << 4, /**< The producer detected an error boundary for this fragment. */
} trace_packet_flags_t;

/** @brief Fixed header prepended to every logical trace packet. */
typedef struct {
    uint8_t version; /**< Packet format version, currently @ref TRACE_PACKET_VERSION. */
    uint8_t type; /**< Protocol family carried by the payload, see @ref trace_type_t. */
    uint8_t channel; /**< Logical capture channel id assigned by the board mapping. */
    uint8_t flags; /**< Bitwise OR of @ref trace_packet_flags_t values. */
    uint16_t payload_len; /**< Number of valid bytes stored in @ref trace_packet_t.payload. */
    uint16_t meta; /**< Protocol-specific auxiliary value such as event count or lane metadata. */
    uint32_t sequence; /**< Monotonic per-session fragment sequence assigned by the producer. */
    uint32_t timestamp_us; /**< Producer-supplied microsecond timestamp for fragment start time. */
} trace_packet_header_t;

/** @brief Serialized byte size of @ref trace_packet_header_t. */
#define TRACE_PACKET_HEADER_BYTES ((uint32_t)sizeof(trace_packet_header_t))

/** @brief Payload bytes available after subtracting the fixed packet header. */
#define TRACE_PACKET_PAYLOAD_BYTES (TRACE_PACKET_BYTES - TRACE_PACKET_HEADER_BYTES)

_Static_assert(TRACE_PACKET_BYTES > sizeof(trace_packet_header_t),
               "TRACE_PACKET_BYTES must be larger than trace_packet_header_t");

/** @brief Complete fixed-size packet stored in the trace ring. */
typedef struct {
    trace_packet_header_t header; /**< Fixed metadata header describing the payload fragment. */
    uint8_t payload[TRACE_PACKET_PAYLOAD_BYTES]; /**< Protocol-specific payload bytes for this fragment. */
} trace_packet_t;

#endif