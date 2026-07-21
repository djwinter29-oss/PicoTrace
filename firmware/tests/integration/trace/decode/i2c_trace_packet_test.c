#include "trace/decode/i2c_trace_packet_test.h"

#include <assert.h>
#include <stdint.h>

#include "trace/decode/i2c/i2c_trace_packet.h"
#include "trace/trace_packet.h"

typedef struct {
    trace_packet_t packets[4];
    uint32_t count;
} test_i2c_packet_capture_t;

typedef struct {
    trace_packet_t packets[4];
    uint32_t count;
    uint32_t reject_count;
} test_i2c_packet_reject_once_t;

static uint32_t test_timestamp_us(void) {
    return 1234u;
}

static bool capture_i2c_packet(void *context, const trace_packet_t *packet) {
    test_i2c_packet_capture_t *capture = (test_i2c_packet_capture_t *)context;

    assert(capture->count < 4u);
    capture->packets[capture->count] = *packet;
    capture->count += 1u;
    return true;
}

static bool reject_first_i2c_packet(void *context, const trace_packet_t *packet) {
    test_i2c_packet_reject_once_t *capture = (test_i2c_packet_reject_once_t *)context;

    if (capture->reject_count == 0u) {
        capture->reject_count = 1u;
        return false;
    }

    assert(capture->count < 4u);
    capture->packets[capture->count] = *packet;
    capture->count += 1u;
    return true;
}

static void test_i2c_trace_packet_builder_fragments_full_transaction(void) {
    i2c_trace_packet_builder_t builder;
    test_i2c_packet_capture_t capture = {0};
    uint32_t max_events = TRACE_PACKET_PAYLOAD_BYTES / I2C_TRACE_EVENT_BYTES;

    assert(i2c_trace_packet_builder_init(&builder, 3u, capture_i2c_packet, &capture, test_timestamp_us) == true);

    for (uint32_t index = 0u; index < max_events; ++index) {
        assert(i2c_trace_packet_builder_append_event(
                   &builder,
                   I2C_DECODE_EVENT_DATA,
                   (uint8_t)index
               ) == true);
    }

    assert(capture.count == 0u);

    assert(i2c_trace_packet_builder_append_event(
               &builder,
               I2C_DECODE_EVENT_STOP,
               0u
           ) == true);

    assert(capture.count == 2u);
    assert(capture.packets[0].header.version == TRACE_PACKET_VERSION);
    assert(capture.packets[0].header.type == TRACE_TYPE_I2C);
    assert(capture.packets[0].header.channel == 3u);
    assert(capture.packets[0].header.flags == 0u);
    assert(capture.packets[0].header.payload_len == TRACE_PACKET_PAYLOAD_BYTES);
    assert(capture.packets[0].header.meta == (uint16_t)max_events);
    assert(capture.packets[0].header.sequence == 1u);
    assert(capture.packets[0].header.timestamp_us == 1234u);
    assert(capture.packets[0].payload[0] == I2C_DECODE_EVENT_DATA);
    assert(capture.packets[0].payload[1] == 0u);
    assert(capture.packets[0].payload[TRACE_PACKET_PAYLOAD_BYTES - 2u] == I2C_DECODE_EVENT_DATA);
    assert(capture.packets[0].payload[TRACE_PACKET_PAYLOAD_BYTES - 1u] == (uint8_t)(max_events - 1u));

    assert(capture.packets[1].header.flags == (TRACE_FLAG_CONTINUED | TRACE_FLAG_END));
    assert(capture.packets[1].header.payload_len == I2C_TRACE_EVENT_BYTES);
    assert(capture.packets[1].header.meta == 1u);
    assert(capture.packets[1].header.sequence == 2u);
    assert(capture.packets[1].payload[0] == I2C_DECODE_EVENT_STOP);
    assert(capture.packets[1].payload[1] == 0u);
    assert(builder.packet_open == false);
    assert(builder.transaction_fragmented == false);
}

static void test_i2c_trace_packet_builder_marks_next_packet_after_overflow(void) {
    i2c_trace_packet_builder_t builder;
    test_i2c_packet_reject_once_t capture = {0};

    assert(i2c_trace_packet_builder_init(&builder, 2u, reject_first_i2c_packet, &capture, test_timestamp_us) == true);

    assert(i2c_trace_packet_builder_append_event(&builder, I2C_DECODE_EVENT_STOP, 0u) == false);
    assert(capture.count == 0u);

    assert(i2c_trace_packet_builder_append_event(&builder, I2C_DECODE_EVENT_STOP, 0u) == true);
    assert(capture.count == 1u);
    assert(capture.packets[0].header.flags == (TRACE_FLAG_OVERFLOW | TRACE_FLAG_END));
    assert(capture.packets[0].header.sequence == 2u);
    assert(capture.packets[0].header.timestamp_us == 1234u);
}

static void test_i2c_trace_packet_builder_marks_explicit_overflow_on_next_packet(void) {
    i2c_trace_packet_builder_t builder;
    test_i2c_packet_capture_t capture = {0};

    assert(i2c_trace_packet_builder_init(&builder, 1u, capture_i2c_packet, &capture, test_timestamp_us) == true);

    i2c_trace_packet_builder_mark_next_packet(&builder, TRACE_FLAG_OVERFLOW);
    assert(i2c_trace_packet_builder_append_event(&builder, I2C_DECODE_EVENT_STOP, 0u) == true);

    assert(capture.count == 1u);
    assert(capture.packets[0].header.flags == (TRACE_FLAG_OVERFLOW | TRACE_FLAG_END));
    assert(capture.packets[0].header.sequence == 1u);
}

static void test_i2c_trace_packet_builder_emits_error_event_fragment_immediately(void) {
    i2c_trace_packet_builder_t builder;
    test_i2c_packet_capture_t capture = {0};

    assert(i2c_trace_packet_builder_init(&builder, 1u, capture_i2c_packet, &capture, test_timestamp_us) == true);
    assert(i2c_trace_packet_builder_append_event(&builder, I2C_DECODE_EVENT_DATA, 0x55u) == true);
    assert(i2c_trace_packet_builder_append_event(
               &builder,
               I2C_DECODE_EVENT_ERROR,
               (uint8_t)I2C_DECODER_RESULT_INVALID_INPUT
           ) == true);

    assert(capture.count == 1u);
    assert(capture.packets[0].header.flags == TRACE_FLAG_END);
    assert(capture.packets[0].header.payload_len == (2u * I2C_TRACE_EVENT_BYTES));
    assert(capture.packets[0].header.meta == 2u);
    assert(capture.packets[0].payload[0] == I2C_DECODE_EVENT_DATA);
    assert(capture.packets[0].payload[1] == 0x55u);
    assert(capture.packets[0].payload[2] == I2C_DECODE_EVENT_ERROR);
    assert(capture.packets[0].payload[3] == (uint8_t)I2C_DECODER_RESULT_INVALID_INPUT);
}

static void test_i2c_trace_packet_builder_emits_control_boundary_fragment_immediately(void) {
    i2c_trace_packet_builder_t builder;
    test_i2c_packet_capture_t capture = {0};

    assert(i2c_trace_packet_builder_init(&builder, 1u, capture_i2c_packet, &capture, test_timestamp_us) == true);
    assert(i2c_trace_packet_builder_append_event(&builder, I2C_DECODE_EVENT_DATA, 0x33u) == true);
    assert(i2c_trace_packet_builder_append_event(&builder, I2C_DECODE_EVENT_CONTROL_RECONFIG, 0u) == true);

    assert(capture.count == 1u);
    assert(capture.packets[0].header.flags == TRACE_FLAG_END);
    assert(capture.packets[0].header.payload_len == (2u * I2C_TRACE_EVENT_BYTES));
    assert(capture.packets[0].header.meta == 2u);
    assert(capture.packets[0].payload[0] == I2C_DECODE_EVENT_DATA);
    assert(capture.packets[0].payload[1] == 0x33u);
    assert(capture.packets[0].payload[2] == I2C_DECODE_EVENT_CONTROL_RECONFIG);
    assert(capture.packets[0].payload[3] == 0u);
}

void run_i2c_trace_packet_tests(void) {
    test_i2c_trace_packet_builder_fragments_full_transaction();
    test_i2c_trace_packet_builder_marks_next_packet_after_overflow();
    test_i2c_trace_packet_builder_marks_explicit_overflow_on_next_packet();
    test_i2c_trace_packet_builder_emits_error_event_fragment_immediately();
    test_i2c_trace_packet_builder_emits_control_boundary_fragment_immediately();
}
