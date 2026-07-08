/**
 * @file i2c_trace_packet.c
 * @brief Helpers for packing decoded I2C events into fixed trace packets.
 */

#include "trace/decode/i2c_trace_packet.h"

#include <stddef.h>
#include <string.h>

static void i2c_trace_packet_builder_reset_open_packet(i2c_trace_packet_builder_t *builder) {
    builder->packet_open = false;
    builder->payload_offset = 0u;
    builder->event_count = 0u;
}

static void i2c_trace_packet_builder_begin_packet(i2c_trace_packet_builder_t *builder) {
    builder->packet.header.version = TRACE_PACKET_VERSION;
    builder->packet.header.type = TRACE_TYPE_I2C;
    builder->packet.header.channel = builder->logical_channel;
    builder->packet.header.flags = builder->pending_flags;
    builder->packet.header.payload_len = 0u;
    builder->packet.header.meta = 0u;
    builder->packet.header.sequence = ++builder->emitted_packets;
    builder->packet.header.timestamp_us =
        (builder->timestamp_source != NULL) ? builder->timestamp_source() : 0u;
    if (builder->transaction_fragmented) {
        builder->packet.header.flags |= TRACE_FLAG_CONTINUED;
    }
    builder->pending_flags = 0u;
    builder->packet_open = true;
}

static bool i2c_trace_packet_builder_flush(i2c_trace_packet_builder_t *builder, bool end_of_transaction) {
    if (!builder->packet_open) {
        return true;
    }

    if (end_of_transaction) {
        builder->packet.header.flags |= TRACE_FLAG_END;
    }
    builder->packet.header.payload_len = (uint16_t)builder->payload_offset;
    builder->packet.header.meta = builder->event_count;
    if (!builder->packet_sink(builder->packet_sink_context, &builder->packet)) {
        i2c_trace_packet_builder_discard(builder);
        i2c_trace_packet_builder_mark_next_packet(builder, TRACE_FLAG_OVERFLOW);
        return false;
    }

    i2c_trace_packet_builder_reset_open_packet(builder);
    builder->transaction_fragmented = !end_of_transaction;
    return true;
}

bool i2c_trace_packet_builder_init(
    i2c_trace_packet_builder_t *builder,
    uint8_t logical_channel,
    i2c_trace_packet_sink_t packet_sink,
    void *packet_sink_context,
    i2c_trace_packet_timestamp_source_t timestamp_source
) {
    if ((builder == NULL) || (packet_sink == NULL)) {
        return false;
    }

    memset(builder, 0, sizeof(*builder));
    builder->logical_channel = logical_channel;
    builder->packet_sink = packet_sink;
    builder->packet_sink_context = packet_sink_context;
    builder->timestamp_source = timestamp_source;
    return true;
}

void i2c_trace_packet_builder_discard(i2c_trace_packet_builder_t *builder) {
    if (builder == NULL) {
        return;
    }

    memset(&builder->packet, 0, sizeof(builder->packet));
    builder->transaction_fragmented = false;
    i2c_trace_packet_builder_reset_open_packet(builder);
}

void i2c_trace_packet_builder_mark_next_packet(i2c_trace_packet_builder_t *builder, uint8_t flags) {
    if (builder == NULL) {
        return;
    }

    builder->pending_flags |= flags;
}

bool i2c_trace_packet_builder_append_event(
    i2c_trace_packet_builder_t *builder,
    uint8_t event_type,
    uint8_t event_value
) {
    uint8_t *payload_slot;

    if ((builder == NULL) || (builder->packet_sink == NULL)) {
        return false;
    }

    if (!builder->packet_open) {
        i2c_trace_packet_builder_begin_packet(builder);
    }

    if ((builder->payload_offset + I2C_TRACE_EVENT_BYTES) > TRACE_PACKET_PAYLOAD_BYTES) {
        if (!i2c_trace_packet_builder_flush(builder, false)) {
            return false;
        }
        i2c_trace_packet_builder_begin_packet(builder);
    }

    payload_slot = &builder->packet.payload[builder->payload_offset];
    i2c_trace_event_write(payload_slot, event_type, event_value);
    builder->payload_offset += I2C_TRACE_EVENT_BYTES;
    builder->event_count += 1u;

    if (event_type == I2C_DECODE_EVENT_STOP) {
        return i2c_trace_packet_builder_flush(builder, true);
    }

    return true;
}

bool i2c_trace_packet_builder_capture_event(void *context, uint8_t event_type, uint8_t event_value) {
    return i2c_trace_packet_builder_append_event(
        (i2c_trace_packet_builder_t *)context,
        event_type,
        event_value
    );
}
