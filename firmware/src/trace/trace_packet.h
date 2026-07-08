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
    TRACE_TYPE_I2C = 1u,
    TRACE_TYPE_SPI = 2u,
} trace_type_t;

/** @brief Packet flags describing boundaries, truncation, or capture errors. */
enum {
    TRACE_FLAG_END = 1u << 0,
    TRACE_FLAG_CONTINUED = 1u << 1,
    TRACE_FLAG_OVERFLOW = 1u << 2,
    TRACE_FLAG_TRUNCATED = 1u << 3,
    TRACE_FLAG_ERROR = 1u << 4,
};

/** @brief Fixed header prepended to every logical trace packet. */
typedef struct {
    uint8_t version;
    uint8_t type;
    uint8_t channel;
    uint8_t flags;
    uint16_t payload_len;
    uint16_t meta;
    uint32_t sequence;
    uint32_t timestamp_us;
} trace_packet_header_t;

/** @brief Serialized byte size of @ref trace_packet_header_t. */
#define TRACE_PACKET_HEADER_BYTES ((uint32_t)sizeof(trace_packet_header_t))

/** @brief Payload bytes available after subtracting the fixed packet header. */
#define TRACE_PACKET_PAYLOAD_BYTES (TRACE_PACKET_BYTES - TRACE_PACKET_HEADER_BYTES)

_Static_assert(TRACE_PACKET_BYTES > sizeof(trace_packet_header_t),
               "TRACE_PACKET_BYTES must be larger than trace_packet_header_t");

/** @brief Complete fixed-size packet stored in the trace ring. */
typedef struct {
    trace_packet_header_t header;
    uint8_t payload[TRACE_PACKET_PAYLOAD_BYTES];
} trace_packet_t;

#endif