/**
 * @file i2c_trace_packet.h
 * @brief Helpers for packing decoded I2C events into fixed trace packets.
 */

#ifndef I2C_TRACE_PACKET_H
#define I2C_TRACE_PACKET_H

#include <stdbool.h>
#include <stdint.h>

#include "trace/i2c/i2c_decoder.h"
#include "trace/trace_packet.h"

/** @brief Serialized byte size of one decoded I2C payload item. */
#define I2C_TRACE_EVENT_BYTES ((uint32_t)sizeof(i2c_decode_event_t))

/**
 * @brief Write one decoded I2C event into a trace-packet payload slot.
 * @param payload Destination pointer to at least @ref I2C_TRACE_EVENT_BYTES writable bytes.
 * @param event_type Event type encoded as @ref i2c_decode_event_type_t.
 * @param event_value Event payload value, such as a byte value or ACK bit state.
 */
static inline void i2c_trace_event_write(uint8_t *payload, uint8_t event_type, uint8_t event_value) {
    payload[0] = event_type;
    payload[1] = event_value;
}

/**
 * @brief Sink invoked when the packet builder completes one trace-packet fragment.
 * @param context Caller-owned callback context.
 * @param packet Completed packet fragment ready for transport.
 * @return `true` to accept the fragment, or `false` to stop because the caller could not accept it.
 */
typedef bool (*i2c_trace_packet_sink_t)(void *context, const trace_packet_t *packet);

/**
 * @brief Callback used to stamp emitted trace packets with a producer timestamp.
 * @return Timestamp in microseconds for the current packet, or `0` when timing is unavailable.
 */
typedef uint32_t (*i2c_trace_packet_timestamp_source_t)(void);

/** @brief Stateful I2C trace-packet assembler carried across decoded events. */
typedef struct {
    trace_packet_t packet; /**< Currently open packet fragment under construction. */
    i2c_trace_packet_sink_t packet_sink; /**< Completed-packet sink used for every emitted fragment. */
    void *packet_sink_context; /**< Caller-owned context passed back to @ref packet_sink. */
    i2c_trace_packet_timestamp_source_t timestamp_source; /**< Optional timestamp source used when opening a fragment. */
    uint8_t logical_channel; /**< Logical I2C channel stored into emitted packet headers. */
    uint8_t pending_flags; /**< Flags applied to the next emitted fragment after a producer-side gap or error. */
    uint32_t emitted_packets; /**< Monotonic packet sequence counter for this I2C channel. */
    uint32_t payload_offset; /**< Number of valid payload bytes already written into @ref packet. */
    uint16_t event_count; /**< Number of decoded I2C events stored in the current fragment. */
    bool packet_open; /**< Indicates whether @ref packet currently contains an in-progress fragment. */
    bool transaction_fragmented; /**< Indicates whether the current transaction already emitted an earlier fragment. */
} i2c_trace_packet_builder_t;

/**
 * @brief Reset and configure one packet builder instance.
 * @param builder Caller-owned packet builder state to initialize.
 * @param logical_channel Logical I2C channel stored in emitted packet headers.
 * @param packet_sink Callback invoked for each completed packet fragment.
 * @param packet_sink_context Caller-owned context passed back to @p packet_sink.
 * @param timestamp_source Optional callback used to stamp packets as they open.
 * @return `true` when the builder was configured, or `false` when the input was invalid.
 */
bool i2c_trace_packet_builder_init(
    i2c_trace_packet_builder_t *builder,
    uint8_t logical_channel,
    i2c_trace_packet_sink_t packet_sink,
    void *packet_sink_context,
    i2c_trace_packet_timestamp_source_t timestamp_source
);

/**
 * @brief Discard any in-progress fragment and reset fragmentation state.
 * @param builder Caller-owned packet builder state to reset.
 */
void i2c_trace_packet_builder_discard(i2c_trace_packet_builder_t *builder);

/**
 * @brief Latch flags onto the next emitted fragment after a producer-side gap or reset.
 * @param builder Caller-owned packet builder state to annotate.
 * @param flags One or more @ref trace_packet_flags_t bits to apply to the next fragment.
 */
void i2c_trace_packet_builder_mark_next_packet(i2c_trace_packet_builder_t *builder, uint8_t flags);

/**
 * @brief Append one decoded I2C event to the current packet builder state.
 * @param builder Caller-owned packet builder state carried across decoded events.
 * @param event_type Event type encoded as @ref i2c_decode_event_type_t.
 * @param event_value Event payload value, such as a byte value or ACK bit state.
 * @return `true` when the event was accepted, or `false` if the input was invalid or the sink
 * rejected one of the emitted packet fragments.
 */
bool i2c_trace_packet_builder_append_event(
    i2c_trace_packet_builder_t *builder,
    uint8_t event_type,
    uint8_t event_value
);

/**
 * @brief Decoder-compatible event sink that appends directly into one packet builder.
 * @param context Builder pointer previously configured by @ref i2c_trace_packet_builder_init.
 * @param event_type Event type encoded as @ref i2c_decode_event_type_t.
 * @param event_value Event payload value, such as a byte value or ACK bit state.
 * @return `true` when the event was accepted, or `false` if the builder could not emit a packet.
 */
bool i2c_trace_packet_builder_capture_event(void *context, uint8_t event_type, uint8_t event_value);

#endif
