#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "tusb.h"

#include "app_control.h"
#include "app_control_test.h"
#include "cli/device_cli.h"
#include "driver/system.h"
#include "test_support.h"
#include "trace/capture/i2c_monitor_control.h"
#include "trace/capture/spi_monitor_control.h"
#include "trace/decode/i2c_decoder_test.h"
#include "trace/decode/i2c_trace_packet_test.h"
#include "trace/trace_ring.h"
#include "usb/usb_cdc.h"
#include "usb/usb_bulk.h"
#include "usb/usb_hid.h"

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
static uint32_t stub_monitor_sample_hz[4];
static bool stub_monitor_running[4];
static bool stub_monitor_overrun[4];
static uint32_t stub_monitor_completed_buffers[4];
static uint32_t stub_monitor_overrun_count[4];
static i2c_monitor_rc_t stub_monitor_result = I2C_MONITOR_RC_OK;
static spi_monitor_capture_t stub_spi_capture[6];
static uint8_t stub_spi_mode[6];
static uint32_t stub_spi_timeout_us[6];
static uint32_t stub_spi_packets_emitted[6];
static uint32_t stub_spi_overrun_count[6];
static bool stub_spi_running[6];
static spi_monitor_rc_t stub_spi_monitor_result = SPI_MONITOR_RC_OK;

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

static i2c_monitor_rc_t stub_monitor_set_channel_sample_hz(uint32_t channel, uint32_t sample_hz) {
    if (stub_monitor_result != I2C_MONITOR_RC_OK) {
        return stub_monitor_result;
    }

    if (channel >= 4u) {
        return I2C_MONITOR_RC_INVALID;
    }

    stub_monitor_sample_hz[channel] = sample_hz;
    stub_monitor_running[channel] = (sample_hz != 0u);
    if (sample_hz == 0u) {
        stub_monitor_completed_buffers[channel] = 0u;
        stub_monitor_overrun[channel] = false;
        stub_monitor_overrun_count[channel] = 0u;
    }

    return I2C_MONITOR_RC_OK;
}

static i2c_monitor_rc_t stub_monitor_get_channel_status(uint32_t channel, i2c_monitor_channel_status_t *status_out) {
    if ((channel >= 4u) || (status_out == NULL)) {
        return I2C_MONITOR_RC_INVALID;
    }

    memset(status_out, 0, sizeof(*status_out));
    status_out->initialized = true;
    status_out->running = stub_monitor_running[channel];
    status_out->overrun = stub_monitor_overrun[channel];
    status_out->transition_pending = false;
    status_out->transition_reason = 0u;
    status_out->sample_hz = stub_monitor_sample_hz[channel];
    status_out->completed_buffers = stub_monitor_completed_buffers[channel];
    status_out->overrun_count = stub_monitor_overrun_count[channel];
    return I2C_MONITOR_RC_OK;
}

static i2c_monitor_rc_t stub_monitor_get_all_status(i2c_monitor_channel_status_t *status_out) {
    if (stub_monitor_result != I2C_MONITOR_RC_OK) {
        return stub_monitor_result;
    }

    if (status_out == NULL) {
        return I2C_MONITOR_RC_INVALID;
    }

    for (uint32_t channel = 0u; channel < 4u; ++channel) {
        memset(&status_out[channel], 0, sizeof(status_out[channel]));
        status_out[channel].initialized = true;
        status_out[channel].running = stub_monitor_running[channel];
        status_out[channel].overrun = stub_monitor_overrun[channel];
        status_out[channel].sample_hz = stub_monitor_sample_hz[channel];
        status_out[channel].completed_buffers = stub_monitor_completed_buffers[channel];
        status_out[channel].overrun_count = stub_monitor_overrun_count[channel];
    }

    return I2C_MONITOR_RC_OK;
}

static spi_monitor_rc_t stub_spi_monitor_set_channel_config(uint32_t channel, const spi_monitor_channel_config_t *config) {
    if (stub_spi_monitor_result != SPI_MONITOR_RC_OK) {
        return stub_spi_monitor_result;
    }

    if ((channel >= 6u) || (config == NULL)) {
        return SPI_MONITOR_RC_INVALID;
    }

    stub_spi_capture[channel] = config->capture;
    stub_spi_mode[channel] = config->spi_mode;
    stub_spi_timeout_us[channel] = (config->timeout_us != 0u) ? config->timeout_us : SPI_MONITOR_TIMEOUT_US_DEFAULT;
    stub_spi_running[channel] = (config->capture != SPI_MONITOR_CAPTURE_DISABLED);
    if (!stub_spi_running[channel]) {
        stub_spi_timeout_us[channel] = 0u;
        stub_spi_packets_emitted[channel] = 0u;
        stub_spi_overrun_count[channel] = 0u;
    }

    return SPI_MONITOR_RC_OK;
}

static spi_monitor_rc_t stub_spi_monitor_get_channel_status(uint32_t channel, spi_monitor_channel_status_t *status_out) {
    if ((channel >= 6u) || (status_out == NULL)) {
        return SPI_MONITOR_RC_INVALID;
    }

    memset(status_out, 0, sizeof(*status_out));
    status_out->initialized = true;
    status_out->running = stub_spi_running[channel];
    status_out->capture = stub_spi_capture[channel];
    status_out->spi_mode = stub_spi_mode[channel];
    status_out->timeout_us = stub_spi_timeout_us[channel];
    status_out->packets_emitted = stub_spi_packets_emitted[channel];
    status_out->overrun_count = stub_spi_overrun_count[channel];
    return SPI_MONITOR_RC_OK;
}

static spi_monitor_rc_t stub_spi_monitor_get_all_status(spi_monitor_channel_status_t *status_out) {
    uint32_t channel;

    if (stub_spi_monitor_result != SPI_MONITOR_RC_OK) {
        return stub_spi_monitor_result;
    }

    if (status_out == NULL) {
        return SPI_MONITOR_RC_INVALID;
    }

    for (channel = 0u; channel < 6u; ++channel) {
        memset(&status_out[channel], 0, sizeof(status_out[channel]));
        status_out[channel].initialized = true;
        status_out[channel].running = stub_spi_running[channel];
        status_out[channel].capture = stub_spi_capture[channel];
        status_out[channel].spi_mode = stub_spi_mode[channel];
        status_out[channel].timeout_us = stub_spi_timeout_us[channel];
        status_out[channel].packets_emitted = stub_spi_packets_emitted[channel];
        status_out[channel].overrun_count = stub_spi_overrun_count[channel];
    }

    return SPI_MONITOR_RC_OK;
}

void reset_usb_stub(void) {
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
    memset(stub_monitor_sample_hz, 0, sizeof(stub_monitor_sample_hz));
    memset(stub_monitor_running, 0, sizeof(stub_monitor_running));
    memset(stub_monitor_overrun, 0, sizeof(stub_monitor_overrun));
    memset(stub_monitor_completed_buffers, 0, sizeof(stub_monitor_completed_buffers));
    memset(stub_monitor_overrun_count, 0, sizeof(stub_monitor_overrun_count));
    stub_monitor_result = I2C_MONITOR_RC_OK;
    memset(stub_spi_capture, 0, sizeof(stub_spi_capture));
    memset(stub_spi_mode, 0, sizeof(stub_spi_mode));
    memset(stub_spi_timeout_us, 0, sizeof(stub_spi_timeout_us));
    memset(stub_spi_packets_emitted, 0, sizeof(stub_spi_packets_emitted));
    memset(stub_spi_overrun_count, 0, sizeof(stub_spi_overrun_count));
    memset(stub_spi_running, 0, sizeof(stub_spi_running));
    stub_spi_monitor_result = SPI_MONITOR_RC_OK;
    memset(stub_vendor_tx_data, 0, sizeof(stub_vendor_tx_data));
    app_control_init();
    i2c_monitor_control_init();
    spi_monitor_control_init();
    i2c_monitor_control_bind_executor(
        stub_monitor_set_channel_sample_hz,
        stub_monitor_get_channel_status,
        stub_monitor_get_all_status
    );
    i2c_monitor_control_set_inline_mode(true);
    spi_monitor_control_bind_executor(
        stub_spi_monitor_set_channel_config,
        stub_spi_monitor_get_channel_status,
        stub_spi_monitor_get_all_status
    );
    spi_monitor_control_set_inline_mode(true);
    stub_led_set_calls = 0u;
    device_cli_init(&test_device_cli_transport);
}

uint32_t test_stub_led_set_calls(void) {
    return stub_led_set_calls;
}

bool test_stub_led_state(void) {
    return stub_led_state;
}

uint32_t test_stub_watchdog_reboot_calls(void) {
    return stub_watchdog_reboot_calls;
}

uint32_t test_stub_watchdog_reboot_pc(void) {
    return stub_watchdog_reboot_pc;
}

uint32_t test_stub_watchdog_reboot_sp(void) {
    return stub_watchdog_reboot_sp;
}

uint32_t test_stub_watchdog_reboot_delay_ms(void) {
    return stub_watchdog_reboot_delay_ms;
}

static void load_cdc_rx(const uint8_t *data, uint32_t length) {
    memcpy(stub_cdc_rx_data, data, length);
    stub_cdc_rx_length = length;
    stub_cdc_rx_offset = 0u;
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

static void test_cli_i2cmon_command_updates_monitor_channel(void) {
    static const uint8_t payload[] = {'i','2','c','m','o','n',' ','2',' ','8','0','0','0','0','0','0','\r'};

    reset_usb_stub();
    load_cdc_rx(payload, sizeof(payload));

    tud_cdc_rx_cb(0u);
    device_cli_poll();

    assert(stub_monitor_running[2] == true);
    assert(stub_monitor_sample_hz[2] == 8000000u);
    assert(strstr((const char *)stub_cdc_tx_data, "i2cmon ch2 running hz=8000000") != NULL);
}

static void test_cli_i2cmon_status_reports_channel_state(void) {
    static const uint8_t payload[] = {'i','2','c','m','o','n',' ','s','t','a','t','u','s',' ','1','\r'};

    reset_usb_stub();
    stub_monitor_running[1] = true;
    stub_monitor_sample_hz[1] = 4000000u;
    stub_monitor_completed_buffers[1] = 12u;
    stub_monitor_overrun[1] = true;
    stub_monitor_overrun_count[1] = 3u;
    load_cdc_rx(payload, sizeof(payload));

    tud_cdc_rx_cb(0u);
    device_cli_poll();

    assert(strstr((const char *)stub_cdc_tx_data, "i2cmon ch1 running hz=4000000 buffers=12 overruns=3 sticky=1") != NULL);
}

static void test_cli_i2cmon_reports_disabled_state(void) {
    static const uint8_t payload[] = {'i','2','c','m','o','n',' ','1',' ','1','0','0','0','0','0','0','\r'};

    reset_usb_stub();
    stub_monitor_result = I2C_MONITOR_RC_DISABLED;
    load_cdc_rx(payload, sizeof(payload));

    tud_cdc_rx_cb(0u);
    device_cli_poll();

    assert(strstr((const char *)stub_cdc_tx_data, "i2cmon disabled") != NULL);
}

static void test_cli_spimon_command_updates_monitor_channel(void) {
    static const uint8_t payload[] = {'s','p','i','m','o','n',' ','4',' ','b','o','t','h',' ','3',' ','2','5','0','0','\r'};

    reset_usb_stub();
    load_cdc_rx(payload, sizeof(payload));

    tud_cdc_rx_cb(0u);
    device_cli_poll();

    assert(stub_spi_running[4] == true);
    assert(stub_spi_capture[4] == SPI_MONITOR_CAPTURE_MOSI_MISO);
    assert(stub_spi_mode[4] == 3u);
    assert(stub_spi_timeout_us[4] == 2500u);
    assert(strstr((const char *)stub_cdc_tx_data, "spimon ch4 running capture=both mode=3 timeout_us=2500") != NULL);
}

static void test_cli_spimon_status_reports_channel_state(void) {
    static const uint8_t payload[] = {'s','p','i','m','o','n',' ','s','t','a','t','u','s',' ','1','\r'};

    reset_usb_stub();
    stub_spi_running[1] = true;
    stub_spi_capture[1] = SPI_MONITOR_CAPTURE_MOSI;
    stub_spi_mode[1] = 0u;
    stub_spi_timeout_us[1] = 1200u;
    stub_spi_packets_emitted[1] = 9u;
    stub_spi_overrun_count[1] = 2u;
    load_cdc_rx(payload, sizeof(payload));

    tud_cdc_rx_cb(0u);
    device_cli_poll();

    assert(strstr((const char *)stub_cdc_tx_data, "spimon ch1 running capture=mosi mode=0 timeout_us=1200 packets=9 overruns=2") != NULL);
}

static void test_cli_spimon_reports_disabled_state(void) {
    static const uint8_t payload[] = {'s','p','i','m','o','n',' ','2',' ','m','o','s','i',' ','1','\r'};

    reset_usb_stub();
    stub_spi_monitor_result = SPI_MONITOR_RC_DISABLED;
    load_cdc_rx(payload, sizeof(payload));

    tud_cdc_rx_cb(0u);
    device_cli_poll();

    assert(strstr((const char *)stub_cdc_tx_data, "spimon disabled") != NULL);
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

static void test_usb_bulk_poll_stream_drains_trace_packet(void) {
    trace_packet_t packet = make_test_trace_packet(31u);
    uint32_t packet_bytes = TRACE_PACKET_HEADER_BYTES + packet.header.payload_len;

    reset_usb_stub();
    trace_ring_init();

    assert(trace_ring_push(&packet) == true);
    usb_bulk_poll_stream(app_control_stream_enabled());
    usb_bulk_flush();

    assert(stub_vendor_tx_length == packet_bytes);
    assert(memcmp(stub_vendor_tx_data, &packet, packet_bytes) == 0);
    assert(trace_ring_available() == 0u);
    assert(stub_vendor_flush_calls == 1u);
}

static void test_usb_bulk_poll_stream_resumes_partial_trace_packet_write(void) {
    trace_packet_t packet = make_test_trace_packet(41u);
    uint32_t packet_bytes = TRACE_PACKET_HEADER_BYTES + packet.header.payload_len;

    reset_usb_stub();
    trace_ring_init();

    assert(trace_ring_push(&packet) == true);
    stub_vendor_available = 8u;
    usb_bulk_poll_stream(app_control_stream_enabled());
    assert(stub_vendor_tx_length == 8u);
    assert(trace_ring_available() == 1u);

    stub_vendor_available = sizeof(stub_vendor_tx_data) - stub_vendor_tx_length;
    usb_bulk_poll_stream(app_control_stream_enabled());
    usb_bulk_flush();

    assert(stub_vendor_tx_length == packet_bytes);
    assert(memcmp(stub_vendor_tx_data, &packet, packet_bytes) == 0);
    assert(trace_ring_available() == 0u);
}

static void test_usb_bulk_poll_stream_restarts_partial_packet_after_stream_disable(void) {
    trace_packet_t packet = make_test_trace_packet(42u);
    uint32_t packet_bytes = TRACE_PACKET_HEADER_BYTES + packet.header.payload_len;

    reset_usb_stub();
    trace_ring_init();

    assert(trace_ring_push(&packet) == true);
    stub_vendor_available = 8u;
    usb_bulk_poll_stream(app_control_stream_enabled());
    assert(stub_vendor_tx_length == 8u);
    assert(trace_ring_available() == 1u);

    app_control_set_stream_enabled(false);
    usb_bulk_poll_stream(app_control_stream_enabled());

    memset(stub_vendor_tx_data, 0, sizeof(stub_vendor_tx_data));
    stub_vendor_tx_length = 0u;
    stub_vendor_available = sizeof(stub_vendor_tx_data);
    app_control_set_stream_enabled(true);
    usb_bulk_poll_stream(app_control_stream_enabled());

    assert(stub_vendor_tx_length == packet_bytes);
    assert(memcmp(stub_vendor_tx_data, &packet, packet_bytes) == 0);
    assert(trace_ring_available() == 0u);
}

static void test_usb_bulk_poll_stream_drops_packet_with_invalid_payload_len(void) {
    trace_packet_t packet = make_test_trace_packet(44u);

    reset_usb_stub();
    trace_ring_init();

    packet.header.payload_len = (uint16_t)(TRACE_PACKET_PAYLOAD_BYTES + 1u);
    assert(trace_ring_push(&packet) == true);
    usb_bulk_poll_stream(app_control_stream_enabled());
    usb_bulk_flush();

    assert(stub_vendor_tx_length == 0u);
    assert(trace_ring_available() == 0u);
    assert(trace_ring_total_consumed() == 1u);
}

static void test_usb_bulk_poll_stream_emits_nothing_when_ring_empty(void) {
    reset_usb_stub();
    usb_bulk_poll_stream(app_control_stream_enabled());
    usb_bulk_flush();

    assert(stub_vendor_tx_length == 0u);
    assert(stub_vendor_flush_calls == 1u);
}

static void test_stream_write_uses_partial_vendor_space(void) {
    uint8_t frame[1024];
    uint32_t index;

    reset_usb_stub();
    stub_vendor_available = 512u;
    for (index = 0u; index < sizeof(frame); ++index) {
        frame[index] = (uint8_t)(index & 0xFFu);
    }

    assert(usb_bulk_stream_write(frame, sizeof(frame)) == 512u);
    assert(stub_vendor_tx_length == 512u);
    assert(memcmp(stub_vendor_tx_data, frame, stub_vendor_tx_length) == 0);
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

static void test_hid_i2c_monitor_set_rate_updates_channel(void) {
    usb_hid_command_t command = {0};
    usb_hid_command_t response = {0};

    reset_usb_stub();
    command.opcode = USB_HID_OPCODE_I2C_MONITOR_SET_RATE;
    command.sequence = 4u;
    command.payload_length = 5u;
    command.payload[0] = 3u;
    command.payload[1] = 0x00u;
    command.payload[2] = 0x12u;
    command.payload[3] = 0x7Au;
    command.payload[4] = 0x00u;

    tud_hid_set_report_cb(0u, 0u, HID_REPORT_TYPE_OUTPUT, (uint8_t const *)&command, sizeof(command));
    usb_hid_poll();

    assert(stub_monitor_running[3] == true);
    assert(stub_monitor_sample_hz[3] == 8000000u);
    assert(tud_hid_get_report_cb(0u, 0u, HID_REPORT_TYPE_INPUT, (uint8_t *)&response, sizeof(response)) == sizeof(response));
    assert(response.opcode == USB_HID_OPCODE_I2C_MONITOR_SET_RATE);
    assert(response.status == USB_HID_STATUS_OK);
}

static void test_hid_i2c_monitor_set_rate_reports_busy(void) {
    usb_hid_command_t command = {0};
    usb_hid_command_t response = {0};

    reset_usb_stub();
    stub_monitor_result = I2C_MONITOR_RC_BUSY;
    command.opcode = USB_HID_OPCODE_I2C_MONITOR_SET_RATE;
    command.sequence = 8u;
    command.payload_length = 5u;
    command.payload[0] = 0u;
    command.payload[1] = 0x40u;
    command.payload[2] = 0x42u;
    command.payload[3] = 0x0Fu;
    command.payload[4] = 0x00u;

    tud_hid_set_report_cb(0u, 0u, HID_REPORT_TYPE_OUTPUT, (uint8_t const *)&command, sizeof(command));
    usb_hid_poll();

    assert(tud_hid_get_report_cb(0u, 0u, HID_REPORT_TYPE_INPUT, (uint8_t *)&response, sizeof(response)) == sizeof(response));
    assert(response.opcode == USB_HID_OPCODE_I2C_MONITOR_SET_RATE);
    assert(response.status == USB_HID_STATUS_BUSY);
}

static void test_hid_i2c_monitor_get_status_returns_payload(void) {
    usb_hid_command_t command = {0};
    usb_hid_command_t response = {0};

    reset_usb_stub();
    stub_monitor_running[0] = true;
    stub_monitor_overrun[0] = true;
    stub_monitor_sample_hz[0] = 2000000u;
    stub_monitor_completed_buffers[0] = 9u;
    stub_monitor_overrun_count[0] = 2u;
    command.opcode = USB_HID_OPCODE_I2C_MONITOR_GET_STATUS;
    command.sequence = 5u;
    command.payload_length = 1u;
    command.payload[0] = 0u;

    tud_hid_set_report_cb(0u, 0u, HID_REPORT_TYPE_OUTPUT, (uint8_t const *)&command, sizeof(command));
    usb_hid_poll();

    assert(tud_hid_get_report_cb(0u, 0u, HID_REPORT_TYPE_INPUT, (uint8_t *)&response, sizeof(response)) == sizeof(response));
    assert(response.opcode == USB_HID_OPCODE_I2C_MONITOR_GET_STATUS);
    assert(response.status == USB_HID_STATUS_OK);
    assert(response.payload_length == 18u);
    assert(response.payload[0] == 0u);
    assert(response.payload[1] == 1u);
    assert(response.payload[2] == 1u);
    assert(response.payload[3] == 1u);
    assert(response.payload[4] == 0x80u);
    assert(response.payload[5] == 0x84u);
    assert(response.payload[6] == 0x1Eu);
    assert(response.payload[7] == 0x00u);
    assert(response.payload[8] == 9u);
    assert(response.payload[12] == 2u);
    assert(response.payload[16] == 0u);
    assert(response.payload[17] == 0u);
}

static void test_hid_i2c_monitor_get_all_status_returns_all_channels(void) {
    usb_hid_command_t command = {0};
    usb_hid_command_t response = {0};

    reset_usb_stub();
    stub_monitor_running[0] = true;
    stub_monitor_sample_hz[0] = 1000000u;
    stub_monitor_overrun_count[0] = 1u;
    stub_monitor_running[2] = true;
    stub_monitor_overrun[2] = true;
    stub_monitor_sample_hz[2] = 8000000u;
    stub_monitor_overrun_count[2] = 4u;
    command.opcode = USB_HID_OPCODE_I2C_MONITOR_GET_ALL_STATUS;
    command.sequence = 6u;

    tud_hid_set_report_cb(0u, 0u, HID_REPORT_TYPE_OUTPUT, (uint8_t const *)&command, sizeof(command));
    usb_hid_poll();

    assert(tud_hid_get_report_cb(0u, 0u, HID_REPORT_TYPE_INPUT, (uint8_t *)&response, sizeof(response)) == sizeof(response));
    assert(response.opcode == USB_HID_OPCODE_I2C_MONITOR_GET_ALL_STATUS);
    assert(response.status == USB_HID_STATUS_OK);
    assert(response.payload_length == 56u);
    assert(response.payload[0] == 0u);
    assert(response.payload[2] == 1u);
    assert(response.payload[4] == 0x40u);
    assert(response.payload[5] == 0x42u);
    assert(response.payload[6] == 0x0Fu);
    assert(response.payload[7] == 0x00u);
    assert(response.payload[8] == 1u);
    assert(response.payload[12] == 0u);
    assert(response.payload[13] == 0u);
    assert(response.payload[28] == 2u);
    assert(response.payload[30] == 1u);
    assert(response.payload[31] == 1u);
    assert(response.payload[32] == 0x00u);
    assert(response.payload[33] == 0x12u);
    assert(response.payload[34] == 0x7Au);
    assert(response.payload[35] == 0x00u);
    assert(response.payload[36] == 4u);
    assert(response.payload[40] == 0u);
    assert(response.payload[41] == 0u);
}

static void test_hid_i2c_monitor_get_all_status_rejects_failed_snapshot(void) {
    usb_hid_command_t command = {0};
    usb_hid_command_t response = {0};

    reset_usb_stub();
    stub_monitor_result = I2C_MONITOR_RC_FAILED;
    command.opcode = USB_HID_OPCODE_I2C_MONITOR_GET_ALL_STATUS;
    command.sequence = 10u;

    tud_hid_set_report_cb(0u, 0u, HID_REPORT_TYPE_OUTPUT, (uint8_t const *)&command, sizeof(command));
    usb_hid_poll();

    assert(tud_hid_get_report_cb(0u, 0u, HID_REPORT_TYPE_INPUT, (uint8_t *)&response, sizeof(response)) == sizeof(response));
    assert(response.opcode == USB_HID_OPCODE_I2C_MONITOR_GET_ALL_STATUS);
    assert(response.status == USB_HID_STATUS_REJECTED);
    assert(response.payload_length == 0u);
}

static void test_hid_spi_monitor_set_config_updates_channel(void) {
    usb_hid_command_t command = {0};
    usb_hid_command_t response = {0};

    reset_usb_stub();
    command.opcode = USB_HID_OPCODE_SPI_MONITOR_SET_CONFIG;
    command.sequence = 11u;
    command.payload_length = 7u;
    command.payload[0] = 4u;
    command.payload[1] = SPI_MONITOR_CAPTURE_MOSI_MISO;
    command.payload[2] = 3u;
    command.payload[3] = 0xC4u;
    command.payload[4] = 0x09u;
    command.payload[5] = 0x00u;
    command.payload[6] = 0x00u;

    tud_hid_set_report_cb(0u, 0u, HID_REPORT_TYPE_OUTPUT, (uint8_t const *)&command, sizeof(command));
    usb_hid_poll();

    assert(stub_spi_running[4] == true);
    assert(stub_spi_capture[4] == SPI_MONITOR_CAPTURE_MOSI_MISO);
    assert(stub_spi_mode[4] == 3u);
    assert(stub_spi_timeout_us[4] == 2500u);
    assert(tud_hid_get_report_cb(0u, 0u, HID_REPORT_TYPE_INPUT, (uint8_t *)&response, sizeof(response)) == sizeof(response));
    assert(response.opcode == USB_HID_OPCODE_SPI_MONITOR_SET_CONFIG);
    assert(response.status == USB_HID_STATUS_OK);
}

static void test_hid_spi_monitor_set_config_reports_busy(void) {
    usb_hid_command_t command = {0};
    usb_hid_command_t response = {0};

    reset_usb_stub();
    stub_spi_monitor_result = SPI_MONITOR_RC_BUSY;
    command.opcode = USB_HID_OPCODE_SPI_MONITOR_SET_CONFIG;
    command.sequence = 12u;
    command.payload_length = 7u;
    command.payload[0] = 0u;
    command.payload[1] = SPI_MONITOR_CAPTURE_MOSI;
    command.payload[2] = 1u;
    command.payload[3] = 0x00u;
    command.payload[4] = 0x00u;
    command.payload[5] = 0x00u;
    command.payload[6] = 0x00u;

    tud_hid_set_report_cb(0u, 0u, HID_REPORT_TYPE_OUTPUT, (uint8_t const *)&command, sizeof(command));
    usb_hid_poll();

    assert(tud_hid_get_report_cb(0u, 0u, HID_REPORT_TYPE_INPUT, (uint8_t *)&response, sizeof(response)) == sizeof(response));
    assert(response.opcode == USB_HID_OPCODE_SPI_MONITOR_SET_CONFIG);
    assert(response.status == USB_HID_STATUS_BUSY);
}

static void test_hid_spi_monitor_get_status_returns_payload(void) {
    usb_hid_command_t command = {0};
    usb_hid_command_t response = {0};

    reset_usb_stub();
    stub_spi_running[1] = true;
    stub_spi_capture[1] = SPI_MONITOR_CAPTURE_MOSI;
    stub_spi_mode[1] = 2u;
    stub_spi_timeout_us[1] = 1800u;
    stub_spi_packets_emitted[1] = 7u;
    stub_spi_overrun_count[1] = 3u;
    command.opcode = USB_HID_OPCODE_SPI_MONITOR_GET_STATUS;
    command.sequence = 13u;
    command.payload_length = 1u;
    command.payload[0] = 1u;

    tud_hid_set_report_cb(0u, 0u, HID_REPORT_TYPE_OUTPUT, (uint8_t const *)&command, sizeof(command));
    usb_hid_poll();

    assert(tud_hid_get_report_cb(0u, 0u, HID_REPORT_TYPE_INPUT, (uint8_t *)&response, sizeof(response)) == sizeof(response));
    assert(response.opcode == USB_HID_OPCODE_SPI_MONITOR_GET_STATUS);
    assert(response.status == USB_HID_STATUS_OK);
    assert(response.payload_length == 17u);
    assert(response.payload[0] == 1u);
    assert(response.payload[1] == 1u);
    assert(response.payload[2] == 1u);
    assert(response.payload[3] == SPI_MONITOR_CAPTURE_MOSI);
    assert(response.payload[4] == 2u);
    assert(response.payload[5] == 0x08u);
    assert(response.payload[6] == 0x07u);
    assert(response.payload[7] == 0x00u);
    assert(response.payload[8] == 0x00u);
    assert(response.payload[9] == 7u);
    assert(response.payload[13] == 3u);
}

static void test_hid_spi_monitor_get_all_status_returns_all_channels(void) {
    usb_hid_command_t command = {0};
    usb_hid_command_t response = {0};

    reset_usb_stub();
    stub_spi_running[0] = true;
    stub_spi_capture[0] = SPI_MONITOR_CAPTURE_MOSI;
    stub_spi_mode[0] = 0u;
    stub_spi_timeout_us[0] = 1000u;
    stub_spi_running[3] = true;
    stub_spi_capture[3] = SPI_MONITOR_CAPTURE_MOSI_MISO;
    stub_spi_mode[3] = 3u;
    stub_spi_timeout_us[3] = 2500u;
    stub_spi_overrun_count[3] = 1u;
    command.opcode = USB_HID_OPCODE_SPI_MONITOR_GET_ALL_STATUS;
    command.sequence = 14u;

    tud_hid_set_report_cb(0u, 0u, HID_REPORT_TYPE_OUTPUT, (uint8_t const *)&command, sizeof(command));
    usb_hid_poll();

    assert(tud_hid_get_report_cb(0u, 0u, HID_REPORT_TYPE_INPUT, (uint8_t *)&response, sizeof(response)) == sizeof(response));
    assert(response.opcode == USB_HID_OPCODE_SPI_MONITOR_GET_ALL_STATUS);
    assert(response.status == USB_HID_STATUS_OK);
    assert(response.payload_length == 60u);
    assert(response.payload[0] == 0u);
    assert(response.payload[2] == 1u);
    assert(response.payload[3] == SPI_MONITOR_CAPTURE_MOSI);
    assert(response.payload[4] == 0u);
    assert(response.payload[5] == 0xE8u);
    assert(response.payload[6] == 0x03u);
    assert(response.payload[9] == 0u);
    assert(response.payload[30] == 3u);
    assert(response.payload[32] == 1u);
    assert(response.payload[33] == SPI_MONITOR_CAPTURE_MOSI_MISO);
    assert(response.payload[34] == 3u);
    assert(response.payload[35] == 0xC4u);
    assert(response.payload[36] == 0x09u);
    assert(response.payload[39] == 1u);
}

static void test_hid_spi_monitor_get_all_status_rejects_failed_snapshot(void) {
    usb_hid_command_t command = {0};
    usb_hid_command_t response = {0};

    reset_usb_stub();
    stub_spi_monitor_result = SPI_MONITOR_RC_FAILED;
    command.opcode = USB_HID_OPCODE_SPI_MONITOR_GET_ALL_STATUS;
    command.sequence = 15u;

    tud_hid_set_report_cb(0u, 0u, HID_REPORT_TYPE_OUTPUT, (uint8_t const *)&command, sizeof(command));
    usb_hid_poll();

    assert(tud_hid_get_report_cb(0u, 0u, HID_REPORT_TYPE_INPUT, (uint8_t *)&response, sizeof(response)) == sizeof(response));
    assert(response.opcode == USB_HID_OPCODE_SPI_MONITOR_GET_ALL_STATUS);
    assert(response.status == USB_HID_STATUS_REJECTED);
    assert(response.payload_length == 0u);
}

int main(void) {
    test_cli_unknown_command_writes_fixed_helper();
    test_cli_help_writes_response_without_connected_flag();
    test_cli_help_is_flushed_after_temporary_tx_backpressure();
    test_cli_led_command_and_hid_led_command_share_action();
    test_cli_reboot_command_uses_system_reboot();
    test_cli_i2cmon_command_updates_monitor_channel();
    test_cli_i2cmon_status_reports_channel_state();
    test_cli_i2cmon_reports_disabled_state();
    test_cli_spimon_command_updates_monitor_channel();
    test_cli_spimon_status_reports_channel_state();
    test_cli_spimon_reports_disabled_state();
    run_app_control_tests();
    test_system_reboot_uses_watchdog_reboot();
    test_system_board_family_reports_supported_family();
    test_trace_ring_push_and_pop_packet();
    test_trace_ring_pop_copy_returns_owned_packet();
    test_trace_ring_pop_copy_advances_past_peeked_packet();
    test_trace_ring_reports_full_and_counts_drop();
    run_i2c_decoder_tests();
    run_i2c_trace_packet_tests();
    test_usb_bulk_poll_stream_drains_trace_packet();
    test_usb_bulk_poll_stream_resumes_partial_trace_packet_write();
    test_usb_bulk_poll_stream_restarts_partial_packet_after_stream_disable();
    test_usb_bulk_poll_stream_drops_packet_with_invalid_payload_len();
    test_usb_bulk_poll_stream_emits_nothing_when_ring_empty();
    test_stream_write_uses_partial_vendor_space();
    test_vendor_write_uses_bulk_in_interface();
    test_hid_builtin_command_returns_status_response();
    test_hid_stream_command_updates_shared_state();
    test_hid_i2c_monitor_set_rate_updates_channel();
    test_hid_i2c_monitor_set_rate_reports_busy();
    test_hid_i2c_monitor_get_status_returns_payload();
    test_hid_i2c_monitor_get_all_status_returns_all_channels();
    test_hid_i2c_monitor_get_all_status_rejects_failed_snapshot();
    test_hid_spi_monitor_set_config_updates_channel();
    test_hid_spi_monitor_set_config_reports_busy();
    test_hid_spi_monitor_get_status_returns_payload();
    test_hid_spi_monitor_get_all_status_returns_all_channels();
    test_hid_spi_monitor_get_all_status_rejects_failed_snapshot();
    test_hid_reboot_command_runs_once();
    return 0;
}