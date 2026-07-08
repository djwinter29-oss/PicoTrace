#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "tusb.h"

#include "app_control.h"
#include "cli/device_cli.h"
#include "driver/system.h"
#include "trace/decode/i2c_decoder.h"
#include "trace/trace_ring.h"
#include "usb_stream.h"
#include "usb/usb_cdc.h"
#include "usb/usb_bulk.h"
#include "usb/usb_hid.h"

#define TEST_BULK_FRAME_SIZE 4096u
#define TEST_BULK_PATTERN_SIZE 8u

static const uint8_t test_bulk_pattern[TEST_BULK_PATTERN_SIZE] = {
    'S', 'T', 'R', 'M', 0xA5u, 0x5Au, 0xC3u, 0x3Cu,
};

static bool stub_ready = true;
static bool stub_cdc_connected = true;
static uint8_t stub_cdc_rx_data[128];
static uint32_t stub_cdc_rx_length;
static uint32_t stub_cdc_rx_offset;
static uint8_t stub_cdc_tx_data[128];
static uint32_t stub_cdc_tx_length;
static uint32_t stub_cdc_flush_calls;
static bool stub_cdc_write_available_forced;
static uint32_t stub_cdc_write_available_value;
static uint32_t stub_vendor_available;
static uint8_t stub_vendor_tx_data[4096];
static uint32_t stub_vendor_tx_length;
static uint32_t stub_vendor_flush_calls;
static uint32_t stub_watchdog_reboot_calls;
static uint32_t stub_watchdog_reboot_pc;
static uint32_t stub_watchdog_reboot_sp;
static uint32_t stub_watchdog_reboot_delay_ms;
static bool stub_led_state;
static uint32_t stub_led_set_calls;

static uint32_t test_cli_read(void *context, uint8_t *data, uint32_t capacity) {
    (void)context;
    return usb_cdc_read(data, capacity);
}

static bool test_cli_write(void *context, const uint8_t *data, uint32_t length) {
    (void)context;
    return usb_cdc_write(data, length);
}

static const cli_shell_transport_t test_device_cli_transport = {
    .read = test_cli_read,
    .write = test_cli_write,
    .context = NULL,
};

bool tud_ready(void) { return stub_ready; }

bool tud_cdc_n_connected(uint8_t itf) {
    (void)itf;
    return stub_cdc_connected;
}

uint32_t tud_cdc_n_write_available(uint8_t itf) {
    (void)itf;

    if (stub_cdc_write_available_forced) {
        return stub_cdc_write_available_value;
    }

    return (uint32_t)(sizeof(stub_cdc_tx_data) - stub_cdc_tx_length);
}

uint32_t tud_cdc_n_write(uint8_t itf, const void *buffer, uint32_t size) {
    (void)itf;
    memcpy(&stub_cdc_tx_data[stub_cdc_tx_length], buffer, size);
    stub_cdc_tx_length += size;
    return size;
}

void tud_cdc_n_write_flush(uint8_t itf) {
    (void)itf;
    stub_cdc_flush_calls += 1u;
}

uint32_t tud_cdc_n_available(uint8_t itf) {
    (void)itf;
    return stub_cdc_rx_length - stub_cdc_rx_offset;
}

uint32_t tud_cdc_n_read(uint8_t itf, void *buffer, uint32_t size) {
    uint32_t available = tud_cdc_n_available(itf);

    if (size > available) {
        size = available;
    }

    memcpy(buffer, &stub_cdc_rx_data[stub_cdc_rx_offset], size);
    stub_cdc_rx_offset += size;
    return size;
}

uint32_t tud_vendor_n_write_available(uint8_t itf) {
    (void)itf;
    return stub_vendor_available;
}

uint32_t tud_vendor_n_write(uint8_t itf, const void *buffer, uint32_t size) {
    (void)itf;
    memcpy(&stub_vendor_tx_data[stub_vendor_tx_length], buffer, size);
    stub_vendor_tx_length += size;
    stub_vendor_available -= size;
    return size;
}

void tud_vendor_n_write_flush(uint8_t itf) {
    (void)itf;
    stub_vendor_flush_calls += 1u;
}

void watchdog_reboot(uint32_t pc, uint32_t sp, uint32_t delay_ms) {
    stub_watchdog_reboot_calls += 1u;
    stub_watchdog_reboot_pc = pc;
    stub_watchdog_reboot_sp = sp;
    stub_watchdog_reboot_delay_ms = delay_ms;
}

void led_set(bool on) {
    stub_led_state = on;
    stub_led_set_calls += 1u;
}

static void reset_usb_stub(void) {
    stub_ready = true;
    stub_cdc_connected = true;
    stub_cdc_rx_length = 0u;
    stub_cdc_rx_offset = 0u;
    stub_cdc_tx_length = 0u;
    stub_cdc_flush_calls = 0u;
    stub_cdc_write_available_forced = false;
    stub_cdc_write_available_value = 0u;
    memset(stub_cdc_rx_data, 0, sizeof(stub_cdc_rx_data));
    memset(stub_cdc_tx_data, 0, sizeof(stub_cdc_tx_data));
    stub_vendor_available = sizeof(stub_vendor_tx_data);
    stub_vendor_tx_length = 0u;
    stub_vendor_flush_calls = 0u;
    stub_watchdog_reboot_calls = 0u;
    stub_watchdog_reboot_pc = 0u;
    stub_watchdog_reboot_sp = 0u;
    stub_watchdog_reboot_delay_ms = 0u;
    stub_led_state = false;
    stub_led_set_calls = 0u;
    memset(stub_vendor_tx_data, 0, sizeof(stub_vendor_tx_data));
    app_control_init();
    stub_led_set_calls = 0u;
    device_cli_init(&test_device_cli_transport);
}

static void load_cdc_rx(const uint8_t *data, uint32_t length) {
    memcpy(stub_cdc_rx_data, data, length);
    stub_cdc_rx_length = length;
    stub_cdc_rx_offset = 0u;
}

static void assert_vendor_pattern(const uint8_t *data, uint32_t length) {
    uint32_t offset;

    for (offset = 0u; offset < length; offset += TEST_BULK_PATTERN_SIZE) {
        assert(memcmp(&data[offset], test_bulk_pattern, TEST_BULK_PATTERN_SIZE) == 0);
    }
}

static void fill_test_bulk_frame(uint8_t *data, uint32_t length) {
    uint32_t offset;

    for (offset = 0u; offset < length; offset += TEST_BULK_PATTERN_SIZE) {
        memcpy(&data[offset], test_bulk_pattern, TEST_BULK_PATTERN_SIZE);
    }
}

static void test_cli_unknown_command_writes_fixed_helper(void) {
    static const uint8_t payload[] = {'b', 'a', 'd', '\r'};

    reset_usb_stub();
    load_cdc_rx(payload, sizeof(payload));

    tud_cdc_rx_cb(0u);
    device_cli_poll();

    assert(strstr((const char *)stub_cdc_tx_data, "Unknown command.") != NULL);
    assert(strstr((const char *)stub_cdc_tx_data, "Commands:") != NULL);
    assert(strstr((const char *)stub_cdc_tx_data, "help") != NULL);
    assert(strstr((const char *)stub_cdc_tx_data, "led on|off") != NULL);
    assert(strstr((const char *)stub_cdc_tx_data, "stream on|off") != NULL);
    assert(strstr((const char *)stub_cdc_tx_data, "reboot") != NULL);
    assert(stub_cdc_flush_calls >= 1u);
}

static void test_cli_help_writes_response_without_connected_flag(void) {
    static const uint8_t payload[] = {'h', 'e', 'l', 'p', '\r'};

    reset_usb_stub();
    stub_cdc_connected = false;
    load_cdc_rx(payload, sizeof(payload));

    tud_cdc_rx_cb(0u);
    device_cli_poll();

    assert(strstr((const char *)stub_cdc_tx_data, "Commands:") != NULL);
    assert(strstr((const char *)stub_cdc_tx_data, "help") != NULL);
    assert(strstr((const char *)stub_cdc_tx_data, "reboot") != NULL);
    assert(stub_cdc_flush_calls >= 1u);
}

static void test_cli_help_is_flushed_after_temporary_tx_backpressure(void) {
    static const uint8_t payload[] = {'h', 'e', 'l', 'p', '\r'};

    reset_usb_stub();
    stub_cdc_write_available_forced = true;
    stub_cdc_write_available_value = 0u;
    load_cdc_rx(payload, sizeof(payload));

    tud_cdc_rx_cb(0u);
    device_cli_poll();

    assert(stub_cdc_tx_length == 0u);
    assert(stub_cdc_flush_calls == 0u);

    stub_cdc_write_available_value = (uint32_t)sizeof(stub_cdc_tx_data);
    usb_cdc_poll_tx();

    assert(strstr((const char *)stub_cdc_tx_data, "Commands:") != NULL);
    assert(strstr((const char *)stub_cdc_tx_data, "help") != NULL);
    assert(stub_cdc_flush_calls >= 1u);
}

static void test_cli_led_command_and_hid_led_command_share_action(void) {
    static const uint8_t cli_payload[] = {'l', 'e', 'd', ' ', 'o', 'n', '\r'};
    usb_hid_command_t hid_command = {0};
    usb_hid_command_t hid_response = {0};

    reset_usb_stub();
    load_cdc_rx(cli_payload, sizeof(cli_payload));

    tud_cdc_rx_cb(0u);
    device_cli_poll();

    assert(stub_led_set_calls == 1u);
    assert(stub_led_state == true);
    assert(strstr((const char *)stub_cdc_tx_data, "LED on") != NULL);

    hid_command.opcode = USB_HID_OPCODE_LED_OFF;
    hid_command.sequence = 2u;
    tud_hid_set_report_cb(0u, 0u, HID_REPORT_TYPE_OUTPUT, (uint8_t const *)&hid_command, sizeof(hid_command));
    usb_hid_poll();

    assert(stub_led_set_calls == 2u);
    assert(stub_led_state == false);
    assert(tud_hid_get_report_cb(0u, 0u, HID_REPORT_TYPE_INPUT, (uint8_t *)&hid_response, sizeof(hid_response)) == sizeof(hid_response));
    assert(hid_response.opcode == USB_HID_OPCODE_LED_OFF);
    assert(hid_response.status == USB_HID_STATUS_OK);
}

static void test_cli_reboot_command_uses_system_reboot(void) {
    static const uint8_t payload[] = {'r', 'e', 'b', 'o', 'o', 't', '\r'};

    reset_usb_stub();
    load_cdc_rx(payload, sizeof(payload));

    tud_cdc_rx_cb(0u);
    device_cli_poll();

    assert(stub_watchdog_reboot_calls == 1u);
    assert(stub_cdc_tx_length == 0u);
    assert(stub_cdc_flush_calls == 0u);
}

static void test_hid_reboot_command_runs_once(void) {
    usb_hid_command_t command = {0};
    usb_hid_command_t response = {0};

    reset_usb_stub();
    command.opcode = USB_HID_OPCODE_REBOOT;
    command.sequence = 9u;

    tud_hid_set_report_cb(0u, 0u, HID_REPORT_TYPE_OUTPUT, (uint8_t const *)&command, sizeof(command));
    usb_hid_poll();
    usb_hid_poll();

    assert(stub_watchdog_reboot_calls == 1u);
    assert(tud_hid_get_report_cb(0u, 0u, HID_REPORT_TYPE_INPUT, (uint8_t *)&response, sizeof(response)) == sizeof(response));
    assert(response.opcode == USB_HID_OPCODE_REBOOT);
    assert(response.sequence == 9u);
    assert(response.status == USB_HID_STATUS_OK);
}

static void test_system_reboot_uses_watchdog_reboot(void) {
    reset_usb_stub();

    system_reboot();

    assert(stub_watchdog_reboot_calls == 1u);
    assert(stub_watchdog_reboot_pc == 0u);
    assert(stub_watchdog_reboot_sp == 0u);
    assert(stub_watchdog_reboot_delay_ms == 0u);
}

static void test_system_board_family_reports_supported_family(void) {
    assert((strcmp(SYSTEM_BOARD_FAMILY, "pico") == 0) || (strcmp(SYSTEM_BOARD_FAMILY, "pico2") == 0));
}

static trace_packet_t make_test_trace_packet(uint8_t sequence_seed) {
    trace_packet_t packet = {0};

    packet.header.version = TRACE_PACKET_VERSION;
    packet.header.type = TRACE_TYPE_SPI;
    packet.header.channel = 2u;
    packet.header.flags = TRACE_FLAG_END;
    packet.header.payload_len = 4u;
    packet.header.meta = 3u;
    packet.header.sequence = sequence_seed;
    packet.header.timestamp_us = 1000u + sequence_seed;
    packet.payload[0] = sequence_seed;
    packet.payload[1] = (uint8_t)(sequence_seed + 1u);
    packet.payload[2] = (uint8_t)(sequence_seed + 2u);
    packet.payload[3] = (uint8_t)(sequence_seed + 3u);

    return packet;
}

static void test_trace_ring_push_and_pop_packet(void) {
    trace_packet_t packet = make_test_trace_packet(7u);
    const trace_packet_t *queued;

    trace_ring_init();

    assert(trace_ring_available() == 0u);
    assert(trace_ring_free() == TRACE_RING_CAPACITY);
    assert(trace_ring_total_produced() == 0u);
    assert(trace_ring_total_consumed() == 0u);
    assert(trace_ring_high_watermark() == 0u);
    assert(trace_ring_push(&packet) == true);
    assert(trace_ring_available() == 1u);
    assert(trace_ring_free() == (TRACE_RING_CAPACITY - 1u));
    assert(trace_ring_total_produced() == 1u);
    assert(trace_ring_total_consumed() == 0u);
    assert(trace_ring_high_watermark() == 1u);

    queued = trace_ring_peek();
    assert(queued != NULL);
    assert(queued->header.version == TRACE_PACKET_VERSION);
    assert(queued->header.sequence == 7u);
    assert(queued->header.timestamp_us == 1007u);
    assert(queued->payload[0] == 7u);
    assert(queued->payload[3] == 10u);

    trace_ring_pop();
    assert(trace_ring_available() == 0u);
    assert(trace_ring_free() == TRACE_RING_CAPACITY);
    assert(trace_ring_total_produced() == 1u);
    assert(trace_ring_total_consumed() == 1u);
    assert(trace_ring_high_watermark() == 1u);
    assert(trace_ring_peek() == NULL);
}

static void test_trace_ring_pop_copy_returns_owned_packet(void) {
    trace_packet_t packet = make_test_trace_packet(11u);
    trace_packet_t copied = {0};

    trace_ring_init();

    assert(trace_ring_pop_copy(&copied) == false);
    assert(trace_ring_push(&packet) == true);
    assert(trace_ring_pop_copy(&copied) == true);
    assert(copied.header.version == TRACE_PACKET_VERSION);
    assert(copied.header.sequence == 11u);
    assert(copied.header.timestamp_us == 1011u);
    assert(copied.payload[0] == 11u);
    assert(copied.payload[3] == 14u);
    assert(trace_ring_available() == 0u);
    assert(trace_ring_total_produced() == 1u);
    assert(trace_ring_total_consumed() == 1u);
    assert(trace_ring_peek() == NULL);
}

static void test_trace_ring_pop_copy_advances_past_peeked_packet(void) {
    trace_packet_t first = make_test_trace_packet(21u);
    trace_packet_t second = make_test_trace_packet(22u);
    trace_packet_t copied = {0};
    const trace_packet_t *queued;

    trace_ring_init();

    assert(trace_ring_push(&first) == true);
    assert(trace_ring_push(&second) == true);

    queued = trace_ring_peek();
    assert(queued != NULL);
    assert(queued->header.sequence == 21u);

    assert(trace_ring_pop_copy(&copied) == true);
    assert(copied.header.sequence == 21u);

    queued = trace_ring_peek();
    assert(queued != NULL);
    assert(queued->header.sequence == 22u);
    assert(trace_ring_available() == 1u);
    assert(trace_ring_total_produced() == 2u);
    assert(trace_ring_total_consumed() == 1u);
}

static void test_trace_ring_reports_full_and_counts_drop(void) {
    trace_packet_t packet = make_test_trace_packet(1u);
    uint32_t index;

    trace_ring_init();

    for (index = 0u; index < TRACE_RING_CAPACITY; ++index) {
        packet.header.sequence = index;
        assert(trace_ring_push(&packet) == true);
    }

    assert(trace_ring_available() == TRACE_RING_CAPACITY);
    assert(trace_ring_free() == 0u);
    assert(trace_ring_total_produced() == TRACE_RING_CAPACITY);
    assert(trace_ring_total_consumed() == 0u);
    assert(trace_ring_high_watermark() == TRACE_RING_CAPACITY);
    assert(trace_ring_push(&packet) == false);
    assert(trace_ring_dropped_packets() == 1u);

    for (index = 0u; index < TRACE_RING_CAPACITY; ++index) {
        const trace_packet_t *queued = trace_ring_peek();
        assert(queued != NULL);
        assert(queued->header.sequence == index);
        trace_ring_pop();
    }

    assert(trace_ring_available() == 0u);
    assert(trace_ring_free() == TRACE_RING_CAPACITY);
    assert(trace_ring_total_produced() == TRACE_RING_CAPACITY);
    assert(trace_ring_total_consumed() == TRACE_RING_CAPACITY);
    assert(trace_ring_high_watermark() == TRACE_RING_CAPACITY);
}

static void append_i2c_sample(uint8_t *samples, uint32_t *sample_count, bool sda, bool scl, uint32_t repeats) {
    uint8_t encoded = (uint8_t)((sda ? 0x01u : 0u) | (scl ? 0x02u : 0u));

    for (uint32_t index = 0u; index < repeats; ++index) {
        samples[*sample_count] = encoded;
        *sample_count += 1u;
    }
}

static void append_i2c_bit(uint8_t *samples, uint32_t *sample_count, uint8_t bit_value) {
    append_i2c_sample(samples, sample_count, bit_value != 0u, false, 2u);
    append_i2c_sample(samples, sample_count, bit_value != 0u, true, 2u);
    append_i2c_sample(samples, sample_count, bit_value != 0u, false, 2u);
}

static uint32_t pack_i2c_samples(uint32_t *raw_words, uint32_t raw_capacity, const uint8_t *samples, uint32_t sample_count) {
    uint32_t word_count = (sample_count + 15u) / 16u;

    assert(word_count <= raw_capacity);
    memset(raw_words, 0, raw_capacity * sizeof(raw_words[0]));

    for (uint32_t sample_index = 0u; sample_index < sample_count; ++sample_index) {
        uint32_t word_index = sample_index / 16u;
        uint32_t shift = 30u - ((sample_index & 0x0Fu) * 2u);
        raw_words[word_index] |= ((uint32_t)(samples[sample_index] & 0x03u) << shift);
    }

    return word_count;
}

typedef struct {
    i2c_decode_event_t events[32];
    uint32_t count;
} test_i2c_event_capture_t;

static bool capture_i2c_event(void *context, uint8_t event_type, uint8_t event_value) {
    test_i2c_event_capture_t *capture = (test_i2c_event_capture_t *)context;

    assert(capture->count < 32u);
    capture->events[capture->count].type = event_type;
    capture->events[capture->count].value = event_value;
    capture->count += 1u;
    return true;
}

static void test_i2c_decoder_emits_events_from_oversampled_buffer(void) {
    i2c_decoder_state_t decoder_state;
    uint8_t samples[256];
    uint32_t raw_words[16];
    uint32_t sample_count = 0u;
    uint32_t raw_word_count;
    test_i2c_event_capture_t capture = {0};

    reset_usb_stub();
    i2c_decoder_init(&decoder_state);

    append_i2c_sample(samples, &sample_count, true, true, 3u);
    append_i2c_sample(samples, &sample_count, false, true, 3u);

    append_i2c_bit(samples, &sample_count, 1u);
    append_i2c_bit(samples, &sample_count, 0u);
    append_i2c_bit(samples, &sample_count, 1u);
    append_i2c_bit(samples, &sample_count, 0u);
    append_i2c_bit(samples, &sample_count, 0u);
    append_i2c_bit(samples, &sample_count, 0u);
    append_i2c_bit(samples, &sample_count, 0u);
    append_i2c_bit(samples, &sample_count, 0u);
    append_i2c_bit(samples, &sample_count, 0u);

    append_i2c_bit(samples, &sample_count, 0u);
    append_i2c_bit(samples, &sample_count, 1u);
    append_i2c_bit(samples, &sample_count, 0u);
    append_i2c_bit(samples, &sample_count, 1u);
    append_i2c_bit(samples, &sample_count, 1u);
    append_i2c_bit(samples, &sample_count, 0u);
    append_i2c_bit(samples, &sample_count, 1u);
    append_i2c_bit(samples, &sample_count, 0u);
    append_i2c_bit(samples, &sample_count, 0u);

    append_i2c_sample(samples, &sample_count, false, false, 2u);
    append_i2c_sample(samples, &sample_count, false, true, 2u);
    append_i2c_sample(samples, &sample_count, true, true, 3u);

    raw_word_count = pack_i2c_samples(raw_words, 16u, samples, sample_count);

    assert(i2c_decoder_process_buffer(&decoder_state, raw_words, raw_word_count, capture_i2c_event, &capture) == true);
    assert(capture.count == 6u);
    assert(capture.events[0].type == I2C_DECODE_EVENT_START);
    assert(capture.events[1].type == I2C_DECODE_EVENT_DATA);
    assert(capture.events[1].value == 0xA0u);
    assert(capture.events[2].type == I2C_DECODE_EVENT_ACK);
    assert(capture.events[2].value == 0u);
    assert(capture.events[3].type == I2C_DECODE_EVENT_DATA);
    assert(capture.events[3].value == 0x5Au);
    assert(capture.events[4].type == I2C_DECODE_EVENT_ACK);
    assert(capture.events[4].value == 0u);
    assert(capture.events[5].type == I2C_DECODE_EVENT_STOP);
}

static void test_usb_stream_drains_trace_packet_before_placeholder(void) {
    trace_packet_t packet = make_test_trace_packet(31u);
    uint32_t packet_bytes = TRACE_PACKET_HEADER_BYTES + packet.header.payload_len;

    reset_usb_stub();
    trace_ring_init();

    assert(trace_ring_push(&packet) == true);
    usb_stream_poll(app_control_stream_enabled(), usb_bulk_stream_write);
    usb_bulk_flush();

    assert(stub_vendor_tx_length == packet_bytes);
    assert(memcmp(stub_vendor_tx_data, &packet, packet_bytes) == 0);
    assert(trace_ring_available() == 0u);
    assert(stub_vendor_flush_calls == 1u);
}

static void test_usb_stream_resumes_partial_trace_packet_write(void) {
    trace_packet_t packet = make_test_trace_packet(41u);
    uint32_t packet_bytes = TRACE_PACKET_HEADER_BYTES + packet.header.payload_len;

    reset_usb_stub();
    trace_ring_init();

    assert(trace_ring_push(&packet) == true);
    stub_vendor_available = 8u;
    usb_stream_poll(app_control_stream_enabled(), usb_bulk_stream_write);
    assert(stub_vendor_tx_length == 8u);
    assert(trace_ring_available() == 1u);

    stub_vendor_available = sizeof(stub_vendor_tx_data) - stub_vendor_tx_length;
    usb_stream_poll(app_control_stream_enabled(), usb_bulk_stream_write);
    usb_bulk_flush();

    assert(stub_vendor_tx_length == packet_bytes);
    assert(memcmp(stub_vendor_tx_data, &packet, packet_bytes) == 0);
    assert(trace_ring_available() == 0u);
}

static void test_poll_emits_example_frame(void) {
    reset_usb_stub();
    usb_stream_poll(app_control_stream_enabled(), usb_bulk_stream_write);
    usb_bulk_flush();
    assert(stub_vendor_tx_length == sizeof(stub_vendor_tx_data));
    assert(stub_vendor_flush_calls == 1u);
    assert_vendor_pattern(stub_vendor_tx_data, stub_vendor_tx_length);
}

static void test_stream_write_uses_partial_vendor_space(void) {
    uint8_t frame[TEST_BULK_FRAME_SIZE];

    reset_usb_stub();
    stub_vendor_available = 512u;
    fill_test_bulk_frame(frame, sizeof(frame));

    assert(usb_bulk_stream_write(frame, sizeof(frame)) == 512u);
    assert(stub_vendor_tx_length == 512u);
    assert_vendor_pattern(stub_vendor_tx_data, stub_vendor_tx_length);
    assert(stub_vendor_available == 0u);
}

static void test_vendor_write_uses_bulk_in_interface(void) {
    static const uint8_t payload[] = {0x10u, 0x20u, 0x30u};

    reset_usb_stub();
    assert(usb_bulk_write(payload, sizeof(payload)) == true);
    assert(stub_vendor_tx_length == sizeof(payload));
    assert(memcmp(stub_vendor_tx_data, payload, sizeof(payload)) == 0);
    assert(stub_vendor_flush_calls == 0u);

    usb_bulk_flush();
    assert(stub_vendor_flush_calls == 1u);
}

static void test_hid_builtin_command_returns_status_response(void) {
    usb_hid_command_t command = {0};
    usb_hid_command_t response = {0};

    reset_usb_stub();
    command.opcode = USB_HID_OPCODE_GET_STATUS;
    command.sequence = 7u;

    tud_hid_set_report_cb(0u, 0u, HID_REPORT_TYPE_OUTPUT, (uint8_t const *)&command, sizeof(command));
    usb_hid_poll();
    assert(tud_hid_get_report_cb(0u, 0u, HID_REPORT_TYPE_INPUT, (uint8_t *)&response, sizeof(response)) == sizeof(response));
    assert(response.opcode == USB_HID_OPCODE_GET_STATUS);
    assert(response.sequence == 7u);
    assert(response.status == USB_HID_STATUS_OK);
    assert(response.payload_length == 1u);
    assert(response.payload[0] == 1u);
}

static void test_hid_stream_command_updates_shared_state(void) {
    usb_hid_command_t command = {0};
    usb_hid_command_t response = {0};

    reset_usb_stub();
    command.opcode = USB_HID_OPCODE_STREAM_DISABLE;
    command.sequence = 3u;

    tud_hid_set_report_cb(0u, 0u, HID_REPORT_TYPE_OUTPUT, (uint8_t const *)&command, sizeof(command));
    usb_hid_poll();
    assert(app_control_stream_enabled() == false);
    assert(tud_hid_get_report_cb(0u, 0u, HID_REPORT_TYPE_INPUT, (uint8_t *)&response, sizeof(response)) == sizeof(response));
    assert(response.opcode == USB_HID_OPCODE_STREAM_DISABLE);
    assert(response.status == USB_HID_STATUS_OK);
}

int main(void) {
    test_cli_unknown_command_writes_fixed_helper();
    test_cli_help_writes_response_without_connected_flag();
    test_cli_help_is_flushed_after_temporary_tx_backpressure();
    test_cli_led_command_and_hid_led_command_share_action();
    test_cli_reboot_command_uses_system_reboot();
    test_system_reboot_uses_watchdog_reboot();
    test_system_board_family_reports_supported_family();
    test_trace_ring_push_and_pop_packet();
    test_trace_ring_pop_copy_returns_owned_packet();
    test_trace_ring_pop_copy_advances_past_peeked_packet();
    test_trace_ring_reports_full_and_counts_drop();
    test_i2c_decoder_emits_events_from_oversampled_buffer();
    test_usb_stream_drains_trace_packet_before_placeholder();
    test_usb_stream_resumes_partial_trace_packet_write();
    test_poll_emits_example_frame();
    test_stream_write_uses_partial_vendor_space();
    test_vendor_write_uses_bulk_in_interface();
    test_hid_builtin_command_returns_status_response();
    test_hid_stream_command_updates_shared_state();
    test_hid_reboot_command_runs_once();
    return 0;
}