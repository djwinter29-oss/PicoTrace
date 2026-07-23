#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "tusb.h"

#include "app_control.h"
#include "app_control_test.h"
#include "cli/device_cli.h"
#include "driver/system.h"
#include "test_support.h"
#include "trace/capture/i2c/i2c_monitor_control.h"
#include "trace/capture/spi/spi_monitor.h"
#include "trace/capture/spi/spi_monitor_control.h"
#include "spi_monitor_test_hooks.h"
#include "trace/decode/i2c_decoder_test.h"
#include "trace/decode/i2c_trace_packet_test.h"
#include "trace/trace_ring.h"
#include "usb/usb_cdc.h"
#include "usb/usb_bulk.h"
#include "usb/usb_hid.h"

static bool stub_ready = true;
uint32_t stub_time_us32;
static bool stub_cdc_connected = true;
static uint8_t stub_cdc_rx_data[256];
static uint32_t stub_cdc_rx_length;
static uint32_t stub_cdc_rx_offset;
static uint8_t stub_cdc_tx_data[256];
static uint32_t stub_cdc_tx_length;
static uint32_t stub_cdc_flush_calls;
static bool stub_cdc_write_available_forced;
static uint32_t stub_cdc_write_available_value;
static uint32_t stub_vendor_available;
static uint8_t stub_vendor_tx_data[4096];
static uint32_t stub_vendor_tx_length;
static uint32_t stub_vendor_write_calls;
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
static spi_monitor_capture_t stub_spi_capture[SPI_MONITOR_CHANNEL_COUNT];
static uint8_t stub_spi_mode[SPI_MONITOR_CHANNEL_COUNT];
static uint8_t stub_spi_channel_select_mask[2];
static spi_monitor_capture_t stub_spi_bus_capture[2];
static uint8_t stub_spi_bus_mode[2];
static bool stub_spi_bus_running[2];
static uint32_t stub_spi_timeout_us[SPI_MONITOR_CHANNEL_COUNT];
static uint32_t stub_spi_bus_timeout_us[2];
static uint32_t stub_spi_packets_emitted[SPI_MONITOR_CHANNEL_COUNT];
static uint32_t stub_spi_transactions_emitted[SPI_MONITOR_CHANNEL_COUNT];
static uint32_t stub_spi_overrun_count[SPI_MONITOR_CHANNEL_COUNT];
static uint32_t stub_spi_timeout_close_count[SPI_MONITOR_BUS_COUNT];
static bool stub_spi_running[SPI_MONITOR_CHANNEL_COUNT];
static spi_monitor_rc_t stub_spi_monitor_result = SPI_MONITOR_RC_OK;
uint64_t stub_gpio_high_mask = UINT64_MAX;
bool stub_dma_configure_fail_next;

static trace_packet_t make_test_trace_packet(uint8_t sequence_seed);

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
    stub_vendor_write_calls += 1u;
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

static spi_monitor_rc_t stub_spi_monitor_set_bus_config(uint32_t bus, const spi_monitor_bus_config_t *config) {
    uint32_t first_channel;
    uint32_t channel;

    if (stub_spi_monitor_result != SPI_MONITOR_RC_OK) {
        return stub_spi_monitor_result;
    }

    if ((bus >= SPI_MONITOR_BUS_COUNT) || (config == NULL)) {
        return SPI_MONITOR_RC_INVALID;
    }

    first_channel = bus * SPI_MONITOR_CS_SLOTS_PER_BUS;
    stub_spi_channel_select_mask[bus] = config->channel_select_mask;
    stub_spi_bus_running[bus] = (config->capture != SPI_MONITOR_CAPTURE_DISABLED);
    stub_spi_bus_capture[bus] = (config->capture != SPI_MONITOR_CAPTURE_DISABLED) ? config->capture : SPI_MONITOR_CAPTURE_DISABLED;
    stub_spi_bus_mode[bus] = (config->capture != SPI_MONITOR_CAPTURE_DISABLED) ? config->spi_mode : 0u;
    stub_spi_bus_timeout_us[bus] = (config->capture != SPI_MONITOR_CAPTURE_DISABLED)
        ? ((config->timeout_us != 0u) ? config->timeout_us : SPI_MONITOR_TIMEOUT_US_DEFAULT)
        : 0u;
    for (channel = first_channel; channel < (first_channel + SPI_MONITOR_CS_SLOTS_PER_BUS); ++channel) {
        uint8_t slot_mask = (uint8_t)(1u << (channel - first_channel));

        stub_spi_capture[channel] = config->capture;
        stub_spi_mode[channel] = config->spi_mode;
        stub_spi_timeout_us[channel] = ((config->capture != SPI_MONITOR_CAPTURE_DISABLED) && ((config->channel_select_mask & slot_mask) != 0u))
            ? ((config->timeout_us != 0u) ? config->timeout_us : SPI_MONITOR_TIMEOUT_US_DEFAULT)
            : 0u;
        stub_spi_running[channel] = ((config->capture != SPI_MONITOR_CAPTURE_DISABLED) && ((config->channel_select_mask & slot_mask) != 0u));
        if (!stub_spi_running[channel]) {
            stub_spi_capture[channel] = SPI_MONITOR_CAPTURE_DISABLED;
            stub_spi_mode[channel] = 0u;
        }
        if (!stub_spi_running[channel]) {
            stub_spi_packets_emitted[channel] = 0u;
            stub_spi_transactions_emitted[channel] = 0u;
            stub_spi_overrun_count[channel] = 0u;
        }
    }

    return SPI_MONITOR_RC_OK;
}

static spi_monitor_rc_t stub_spi_monitor_get_bus_status(uint32_t bus, spi_monitor_bus_status_t *status_out) {
    uint32_t first_channel;

    if ((bus >= SPI_MONITOR_BUS_COUNT) || (status_out == NULL)) {
        return SPI_MONITOR_RC_INVALID;
    }

    first_channel = bus * SPI_MONITOR_CS_SLOTS_PER_BUS;
    memset(status_out, 0, sizeof(*status_out));
    status_out->initialized = true;
    status_out->running = stub_spi_bus_running[bus];
    status_out->capture = stub_spi_bus_capture[bus];
    status_out->spi_mode = stub_spi_bus_mode[bus];
    status_out->channel_select_mask = stub_spi_channel_select_mask[bus];
    status_out->timeout_us = stub_spi_bus_timeout_us[bus];
    status_out->packets_emitted = stub_spi_packets_emitted[first_channel];
    status_out->transactions_emitted = stub_spi_transactions_emitted[first_channel];
    status_out->overrun_count = stub_spi_overrun_count[first_channel];
    status_out->timeout_close_count = stub_spi_timeout_close_count[bus];
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

    for (channel = 0u; channel < SPI_MONITOR_CHANNEL_COUNT; ++channel) {
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
    stub_vendor_write_calls = 0u;
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
    memset(stub_spi_channel_select_mask, 0, sizeof(stub_spi_channel_select_mask));
    memset(stub_spi_bus_capture, 0, sizeof(stub_spi_bus_capture));
    memset(stub_spi_bus_mode, 0, sizeof(stub_spi_bus_mode));
    memset(stub_spi_bus_running, 0, sizeof(stub_spi_bus_running));
    memset(stub_spi_timeout_us, 0, sizeof(stub_spi_timeout_us));
    memset(stub_spi_bus_timeout_us, 0, sizeof(stub_spi_bus_timeout_us));
    memset(stub_spi_packets_emitted, 0, sizeof(stub_spi_packets_emitted));
    memset(stub_spi_transactions_emitted, 0, sizeof(stub_spi_transactions_emitted));
    memset(stub_spi_overrun_count, 0, sizeof(stub_spi_overrun_count));
    memset(stub_spi_timeout_close_count, 0, sizeof(stub_spi_timeout_close_count));
    memset(stub_spi_running, 0, sizeof(stub_spi_running));
    stub_spi_monitor_result = SPI_MONITOR_RC_OK;
    stub_dma_configure_fail_next = false;
    memset(stub_vendor_tx_data, 0, sizeof(stub_vendor_tx_data));
    usb_bulk_service_stream(false);
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
        stub_spi_monitor_set_bus_config,
        stub_spi_monitor_get_bus_status,
        stub_spi_monitor_get_all_status
    );
    spi_monitor_control_set_inline_mode(true);
    stub_led_set_calls = 0u;
    usb_cdc_reset_stats();
    usb_hid_reset_stats();
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

static void reset_real_spi_monitor_state(void) {
    spi_monitor_bus_config_t config = {0};

    stub_time_us32 = 0u;
    stub_gpio_high_mask = UINT64_MAX;
    stub_dma_configure_fail_next = false;
    app_control_set_stream_enabled(true);
    (void)spi_monitor_init();
    config.capture = SPI_MONITOR_CAPTURE_DISABLED;
    for (uint32_t bus = 0u; bus < SPI_MONITOR_BUS_COUNT; ++bus) {
        assert(spi_monitor_set_bus_config(bus, &config) == SPI_MONITOR_RC_OK);
    }
}

static uint32_t pack_spi_sample_word(const uint8_t *samples, uint32_t sample_count) {
    uint32_t word = 0u;

    for (uint32_t sample = 0u; sample < sample_count; ++sample) {
        word |= ((uint32_t)(samples[sample] & 0x03u)) << (14u - (sample * 2u));
    }

    return word;
}

static uint32_t pack_spi_byte_word(uint8_t mosi_byte, uint8_t miso_byte) {
    uint8_t samples[8];

    for (uint32_t bit = 0u; bit < 8u; ++bit) {
        uint8_t mosi = (uint8_t)((mosi_byte >> (7u - bit)) & 0x01u);
        uint8_t miso = (uint8_t)((miso_byte >> (7u - bit)) & 0x01u);

        samples[bit] = (uint8_t)(mosi | (miso << 1u));
    }

    return pack_spi_sample_word(samples, 8u);
}

static uint32_t pack_spi_mosi_word(uint8_t mosi_byte) {
    return mosi_byte;
}

static uint32_t pack_spi_lane_word(uint8_t data_byte, uint8_t sample_bit_index) {
    uint8_t samples[8];

    for (uint32_t bit = 0u; bit < 8u; ++bit) {
        samples[bit] = (uint8_t)(((data_byte >> (7u - bit)) & 0x01u) << sample_bit_index);
    }

    return pack_spi_sample_word(samples, 8u);
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

static void test_spi_monitor_bus_config_updates_all_bus_channels(void) {
    spi_monitor_bus_config_t config = {0};
    spi_monitor_channel_status_t status[SPI_MONITOR_CHANNEL_COUNT];

    reset_real_spi_monitor_state();
    config.capture = SPI_MONITOR_CAPTURE_MOSI_MISO;
    config.spi_mode = 3u;
    config.channel_select_mask = SPI_MONITOR_CHANNEL_SELECT_ALL;
    config.timeout_us = 2500u;

    assert(spi_monitor_set_bus_config(1u, &config) == SPI_MONITOR_RC_OK);
    assert(spi_monitor_get_all_status(status) == SPI_MONITOR_RC_OK);
    assert(status[2].running == true);
    assert(status[3].running == true);
    assert(status[2].capture == SPI_MONITOR_CAPTURE_MOSI_MISO);
    assert(status[3].capture == SPI_MONITOR_CAPTURE_MOSI_MISO);
    assert(status[2].spi_mode == 3u);
    assert(status[3].spi_mode == 3u);
}

static void test_spi_monitor_allows_different_buses_to_use_different_modes(void) {
    spi_monitor_bus_config_t config0 = {0};
    spi_monitor_bus_config_t config1 = {0};
    spi_monitor_bus_status_t status0;
    spi_monitor_bus_status_t status1;

    reset_real_spi_monitor_state();
    config0.capture = SPI_MONITOR_CAPTURE_MOSI;
    config0.spi_mode = 0u;
    config0.channel_select_mask = 0x01u;
    config1.capture = SPI_MONITOR_CAPTURE_MOSI_MISO;
    config1.spi_mode = 3u;
    config1.channel_select_mask = SPI_MONITOR_CHANNEL_SELECT_ALL;

    assert(spi_monitor_set_bus_config(0u, &config0) == SPI_MONITOR_RC_OK);
    assert(spi_monitor_set_bus_config(1u, &config1) == SPI_MONITOR_RC_OK);
    assert(spi_monitor_get_bus_status(0u, &status0) == SPI_MONITOR_RC_OK);
    assert(spi_monitor_get_bus_status(1u, &status1) == SPI_MONITOR_RC_OK);
    assert(status0.running == true);
    assert(status1.running == true);
    assert(status0.spi_mode == 0u);
    assert(status1.spi_mode == 3u);
}

static void test_spi_monitor_stopping_bus_clears_all_bus_channels(void) {
    spi_monitor_bus_config_t start = {0};
    spi_monitor_bus_config_t stop = {0};
    spi_monitor_channel_status_t status[SPI_MONITOR_CHANNEL_COUNT];

    reset_real_spi_monitor_state();
    start.capture = SPI_MONITOR_CAPTURE_MOSI;
    start.spi_mode = 2u;
    start.channel_select_mask = SPI_MONITOR_CHANNEL_SELECT_ALL;
    stop.capture = SPI_MONITOR_CAPTURE_DISABLED;

    assert(spi_monitor_set_bus_config(0u, &start) == SPI_MONITOR_RC_OK);
    assert(spi_monitor_set_bus_config(0u, &stop) == SPI_MONITOR_RC_OK);
    assert(spi_monitor_get_all_status(status) == SPI_MONITOR_RC_OK);
    assert(status[0].running == false);
    assert(status[1].running == false);
    assert(status[2].running == false);
    assert(status[3].running == false);
}

static void test_spi_monitor_bus_config_can_select_one_channel(void) {
    spi_monitor_bus_config_t config = {0};
    spi_monitor_bus_status_t bus_status;
    spi_monitor_channel_status_t status[SPI_MONITOR_CHANNEL_COUNT];

    reset_real_spi_monitor_state();
    config.capture = SPI_MONITOR_CAPTURE_MOSI;
    config.spi_mode = 1u;
    config.channel_select_mask = 0x02u;
    config.timeout_us = 900u;

    assert(spi_monitor_set_bus_config(0u, &config) == SPI_MONITOR_RC_OK);
    assert(spi_monitor_get_bus_status(0u, &bus_status) == SPI_MONITOR_RC_OK);
    assert(spi_monitor_get_all_status(status) == SPI_MONITOR_RC_OK);
    assert(bus_status.running == true);
    assert(bus_status.channel_select_mask == 0x02u);
    assert(status[0].running == false);
    assert(status[1].running == true);
    assert(status[1].capture == SPI_MONITOR_CAPTURE_MOSI);
    assert(status[1].spi_mode == 1u);
    assert(status[1].timeout_us == 900u);
    assert(status[2].running == false);
    assert(status[1].capture == SPI_MONITOR_CAPTURE_MOSI);
    assert(status[1].spi_mode == 1u);
    assert(status[1].timeout_us == 900u);
}

static void test_spi_monitor_bus_config_rejects_empty_channel_selection(void) {
    spi_monitor_bus_config_t config = {0};

    reset_real_spi_monitor_state();
    config.capture = SPI_MONITOR_CAPTURE_MOSI;
    config.spi_mode = 0u;
    config.channel_select_mask = 0u;

    assert(spi_monitor_set_bus_config(0u, &config) == SPI_MONITOR_RC_INVALID);
}

static void test_spi_monitor_emits_mosi_miso_trace_packet(void) {
    spi_monitor_bus_config_t config = {0};
    spi_monitor_channel_status_t status[SPI_MONITOR_CHANNEL_COUNT];
    trace_packet_t packet = {0};
    uint32_t mosi_words[1];
    uint32_t miso_words[1];

    reset_real_spi_monitor_state();
    trace_ring_init();
    config.capture = SPI_MONITOR_CAPTURE_MOSI_MISO;
    config.spi_mode = 0u;
    config.channel_select_mask = 0x02u;
    config.timeout_us = 1000u;
    mosi_words[0] = pack_spi_lane_word(0xA5u, 0u);
    miso_words[0] = pack_spi_lane_word(0x3Cu, 1u);

    assert(spi_monitor_set_bus_config(0u, &config) == SPI_MONITOR_RC_OK);
    assert(spi_monitor_test_feed_mosi_miso_samples(0u, 0x02u, 100u, mosi_words, miso_words, 1u) == true);
    assert(trace_ring_available() == 0u);

    spi_monitor_test_poll_timeout(0u, 1200u);
    assert(trace_ring_available() == 0u);
    spi_monitor_test_poll_timeout(0u, 1300u);

    assert(trace_ring_available() == 1u);
    assert(trace_ring_pop_copy(&packet) == true);
    assert(packet.header.version == TRACE_PACKET_VERSION);
    assert(packet.header.type == TRACE_TYPE_SPI);
    assert(packet.header.channel == SPI_MONITOR_CH1_LOGICAL_CHANNEL);
    assert((packet.header.flags & TRACE_FLAG_END) != 0u);
    assert(packet.header.payload_len == 2u);
    assert(packet.header.meta == SPI_MONITOR_CAPTURE_MOSI_MISO);
    assert(packet.header.timestamp_us == 100u);
    assert(packet.payload[0] == 0xA5u);
    assert(packet.payload[1] == 0x3Cu);
    assert(spi_monitor_get_all_status(status) == SPI_MONITOR_RC_OK);
    assert(status[1].packets_emitted == 1u);
}

static void test_spi_monitor_emits_mosi_only_trace_packet(void) {
    spi_monitor_bus_config_t config = {0};
    trace_packet_t packet = {0};
    uint32_t raw_words[1];

    reset_real_spi_monitor_state();
    trace_ring_init();
    config.capture = SPI_MONITOR_CAPTURE_MOSI;
    config.spi_mode = 1u;
    config.channel_select_mask = 0x02u;
    config.timeout_us = 800u;
    raw_words[0] = pack_spi_mosi_word(0x5Au);

    assert(spi_monitor_set_bus_config(0u, &config) == SPI_MONITOR_RC_OK);
    assert(spi_monitor_test_feed_samples(0u, 0x02u, 50u, raw_words, 1u) == true);

    spi_monitor_test_poll_timeout(0u, 900u);
    assert(trace_ring_available() == 0u);
    spi_monitor_test_poll_timeout(0u, 950u);

    assert(trace_ring_available() == 1u);
    assert(trace_ring_pop_copy(&packet) == true);
    assert(packet.header.type == TRACE_TYPE_SPI);
    assert(packet.header.channel == SPI_MONITOR_CH1_LOGICAL_CHANNEL);
    assert((packet.header.flags & TRACE_FLAG_END) != 0u);
    assert(packet.header.payload_len == 1u);
    assert(packet.header.meta == SPI_MONITOR_CAPTURE_MOSI);
    assert(packet.payload[0] == 0x5Au);
}

static void test_spi_monitor_stream_disable_blocks_ring_output(void) {
    spi_monitor_bus_config_t config = {0};
    spi_monitor_channel_status_t status[SPI_MONITOR_CHANNEL_COUNT];
    uint32_t raw_words[1];

    reset_real_spi_monitor_state();
    trace_ring_init();
    config.capture = SPI_MONITOR_CAPTURE_MOSI;
    config.spi_mode = 0u;
    config.channel_select_mask = 0x01u;
    config.timeout_us = 1000u;
    raw_words[0] = pack_spi_mosi_word(0x33u);

    assert(spi_monitor_set_bus_config(0u, &config) == SPI_MONITOR_RC_OK);
    app_control_set_stream_enabled(false);
    assert(spi_monitor_test_feed_samples(0u, 0x01u, 25u, raw_words, 1u) == true);
    spi_monitor_test_poll_timeout(0u, 2000u);
    assert(trace_ring_available() == 0u);
    assert(spi_monitor_get_all_status(status) == SPI_MONITOR_RC_OK);
    assert(status[0].packets_emitted == 0u);
}

static void test_spi_monitor_set_bus_config_reports_disabled_when_stream_off(void) {
    spi_monitor_bus_config_t config = {0};

    reset_real_spi_monitor_state();
    app_control_set_stream_enabled(false);
    config.capture = SPI_MONITOR_CAPTURE_MOSI;
    config.spi_mode = 0u;
    config.channel_select_mask = 0x01u;

    assert(spi_monitor_set_bus_config(0u, &config) == SPI_MONITOR_RC_DISABLED);
}

/** @brief Verify that disabling a bus ignores stale SPI mode and channel-selection fields. */
static void test_spi_monitor_disable_ignores_mode_and_channel_validation(void) {
    spi_monitor_bus_config_t start = {0};
    spi_monitor_bus_config_t stop = {0};
    spi_monitor_bus_status_t bus_status;
    spi_monitor_channel_status_t status[SPI_MONITOR_CHANNEL_COUNT];

    reset_real_spi_monitor_state();
    start.capture = SPI_MONITOR_CAPTURE_MOSI;
    start.spi_mode = 0u;
    start.channel_select_mask = 0x03u;
    start.timeout_us = 1000u;
    stop.capture = SPI_MONITOR_CAPTURE_DISABLED;
    stop.spi_mode = 9u;
    stop.channel_select_mask = 0u;

    assert(spi_monitor_set_bus_config(0u, &start) == SPI_MONITOR_RC_OK);
    assert(spi_monitor_set_bus_config(0u, &stop) == SPI_MONITOR_RC_OK);
    assert(spi_monitor_get_bus_status(0u, &bus_status) == SPI_MONITOR_RC_OK);
    assert(spi_monitor_get_all_status(status) == SPI_MONITOR_RC_OK);
    assert(bus_status.running == false);
    assert(status[0].running == false);
    assert(status[1].running == false);
}

/** @brief Verify that SPI reconfigure failure leaves the bus stopped, matching I2C stop-first recovery. */
static void test_spi_monitor_reconfig_failure_leaves_bus_stopped(void) {
    spi_monitor_bus_config_t start = {0};
    spi_monitor_bus_config_t reconfig = {0};
    spi_monitor_bus_status_t bus_status;
    spi_monitor_channel_status_t status[SPI_MONITOR_CHANNEL_COUNT];

    reset_real_spi_monitor_state();
    start.capture = SPI_MONITOR_CAPTURE_MOSI;
    start.spi_mode = 0u;
    start.channel_select_mask = 0x01u;
    start.timeout_us = 1000u;
    reconfig.capture = SPI_MONITOR_CAPTURE_MOSI_MISO;
    reconfig.spi_mode = 1u;
    reconfig.channel_select_mask = 0x03u;
    reconfig.timeout_us = 900u;

    assert(spi_monitor_set_bus_config(0u, &start) == SPI_MONITOR_RC_OK);
    stub_dma_configure_fail_next = true;
    assert(spi_monitor_set_bus_config(0u, &reconfig) == SPI_MONITOR_RC_FAILED);
    assert(spi_monitor_get_bus_status(0u, &bus_status) == SPI_MONITOR_RC_OK);
    assert(spi_monitor_get_all_status(status) == SPI_MONITOR_RC_OK);
    assert(bus_status.running == false);
    assert(bus_status.capture == SPI_MONITOR_CAPTURE_DISABLED);
    assert(status[0].running == false);
    assert(status[1].running == false);
    assert(status[2].running == false);
}

static void test_spi_monitor_invalid_reconfig_does_not_flush_transaction(void) {
    spi_monitor_bus_config_t start = {0};
    spi_monitor_bus_config_t invalid = {0};
    trace_packet_t packet = {0};
    uint32_t raw_words[1];

    reset_real_spi_monitor_state();
    trace_ring_init();
    start.capture = SPI_MONITOR_CAPTURE_MOSI;
    start.spi_mode = 2u;
    start.channel_select_mask = 0x01u;
    start.timeout_us = 1000u;
    invalid.capture = SPI_MONITOR_CAPTURE_MOSI;
    invalid.spi_mode = 2u;
    invalid.channel_select_mask = 0u;
    raw_words[0] = pack_spi_mosi_word(0x96u);

    assert(spi_monitor_set_bus_config(0u, &start) == SPI_MONITOR_RC_OK);
    assert(spi_monitor_test_feed_samples(0u, 0x01u, 100u, raw_words, 1u) == true);
    assert(trace_ring_available() == 0u);
    assert(spi_monitor_set_bus_config(0u, &invalid) == SPI_MONITOR_RC_INVALID);
    assert(trace_ring_available() == 0u);

    spi_monitor_test_poll_timeout(0u, 1500u);
    assert(trace_ring_available() == 0u);
    spi_monitor_test_poll_timeout(0u, 1600u);

    assert(trace_ring_available() == 1u);
    assert(trace_ring_pop_copy(&packet) == true);
    assert(packet.header.channel == SPI_MONITOR_CH0_LOGICAL_CHANNEL);
    assert(packet.payload[0] == 0x96u);
}

/** @brief Verify that an open SPI transaction does not increment emitted status before ring flush. */
static void test_spi_monitor_open_transaction_does_not_count_packet_before_ring_push(void) {
    spi_monitor_bus_config_t config = {0};
    spi_monitor_channel_status_t status[SPI_MONITOR_CHANNEL_COUNT];
    uint32_t raw_words[1];

    reset_real_spi_monitor_state();
    trace_ring_init();
    config.capture = SPI_MONITOR_CAPTURE_MOSI;
    config.spi_mode = 0u;
    config.channel_select_mask = 0x01u;
    config.timeout_us = 1000u;
    raw_words[0] = pack_spi_mosi_word(0x33u);

    assert(spi_monitor_set_bus_config(0u, &config) == SPI_MONITOR_RC_OK);
    assert(spi_monitor_test_feed_samples(0u, 0x01u, 25u, raw_words, 1u) == true);
    assert(spi_monitor_get_all_status(status) == SPI_MONITOR_RC_OK);
    assert(trace_ring_available() == 0u);
    assert(status[0].packets_emitted == 0u);

    spi_monitor_test_poll_timeout(0u, 2000u);
    assert(trace_ring_available() == 0u);
    spi_monitor_test_poll_timeout(0u, 2100u);

    assert(spi_monitor_get_all_status(status) == SPI_MONITOR_RC_OK);
    assert(status[0].packets_emitted == 1u);
}

/** @brief Verify that words sampled after CS deasserts are dropped instead of extending the transaction. */
static void test_spi_monitor_idle_cs_handoff_drops_inactive_words(void) {
    spi_monitor_bus_config_t config = {0};
    trace_packet_t packet = {0};
    uint32_t raw_words[1];

    reset_real_spi_monitor_state();
    trace_ring_init();
    config.capture = SPI_MONITOR_CAPTURE_MOSI;
    config.spi_mode = 0u;
    config.channel_select_mask = 0x01u;
    config.timeout_us = 1000u;
    raw_words[0] = pack_spi_mosi_word(0x12u);

    assert(spi_monitor_set_bus_config(0u, &config) == SPI_MONITOR_RC_OK);
    assert(spi_monitor_test_feed_samples(0u, 0x01u, 100u, raw_words, 1u) == true);
    raw_words[0] = pack_spi_mosi_word(0x34u);
    assert(spi_monitor_test_feed_samples(0u, 0x00u, 200u, raw_words, 1u) == true);

    spi_monitor_test_poll_timeout(0u, 1500u);
    assert(trace_ring_available() == 0u);
    spi_monitor_test_poll_timeout(0u, 1600u);

    assert(trace_ring_available() == 1u);
    assert(trace_ring_pop_copy(&packet) == true);
    assert((packet.header.flags & TRACE_FLAG_END) != 0u);
    assert(packet.header.payload_len == 1u);
    assert(packet.payload[0] == 0x12u);
}

/** @brief Verify that poll-driven DMA progress emits a short SPI transfer without waiting for a full half-buffer. */
static void test_spi_monitor_poll_flushes_short_dma_progress(void) {
    spi_monitor_bus_config_t config = {0};
    trace_packet_t packet = {0};
    uint32_t raw_words[1];

    reset_real_spi_monitor_state();
    trace_ring_init();
    config.capture = SPI_MONITOR_CAPTURE_MOSI;
    config.spi_mode = 0u;
    config.channel_select_mask = 0x01u;
    config.timeout_us = 1000u;

    assert(spi_monitor_set_bus_config(0u, &config) == SPI_MONITOR_RC_OK);
    raw_words[0] = pack_spi_mosi_word(0x9Au);
    assert(spi_monitor_test_stage_channel_dma_progress(0u, raw_words, 1u) == true);

    stub_gpio_set_level(SPI_MONITOR_SPI0_CS0_GPIO, false);
    stub_time_us32 = 100u;
    spi_monitor_poll();

    assert(trace_ring_available() == 0u);

    stub_gpio_set_level(SPI_MONITOR_SPI0_CS0_GPIO, true);
    stub_gpio_fire_irq(SPI_MONITOR_SPI0_CS0_GPIO, GPIO_IRQ_EDGE_RISE);
    stub_time_us32 = 1200u;
    spi_monitor_poll();
    assert(trace_ring_available() == 0u);

    stub_time_us32 = 1300u;
    spi_monitor_poll();

    assert(trace_ring_available() == 1u);
    assert(trace_ring_pop_copy(&packet) == true);
    assert(packet.header.type == TRACE_TYPE_SPI);
    assert(packet.header.channel == SPI_MONITOR_CH0_LOGICAL_CHANNEL);
    assert((packet.header.flags & TRACE_FLAG_END) != 0u);
    assert(packet.header.payload_len == 1u);
    assert(packet.payload[0] == 0x9Au);
}

/** @brief Verify that same-bus selected channels can retire distinct staged DMA data independently. */
static void test_spi_monitor_poll_flushes_independent_same_bus_channel_dma_progress(void) {
    spi_monitor_bus_config_t config = {0};
    trace_packet_t packet = {0};
    uint32_t channel0_words[1];
    uint32_t channel1_words[1];

    reset_real_spi_monitor_state();
    trace_ring_init();
    config.capture = SPI_MONITOR_CAPTURE_MOSI;
    config.spi_mode = 0u;
    config.channel_select_mask = SPI_MONITOR_CHANNEL_SELECT_ALL;
    config.timeout_us = 1000u;

    assert(spi_monitor_set_bus_config(0u, &config) == SPI_MONITOR_RC_OK);
    channel0_words[0] = pack_spi_mosi_word(0x11u);
    channel1_words[0] = pack_spi_mosi_word(0x22u);
    assert(spi_monitor_test_stage_channel_dma_progress(0u, channel0_words, 1u) == true);
    assert(spi_monitor_test_stage_channel_dma_progress(1u, channel1_words, 1u) == true);

    stub_gpio_set_level(SPI_MONITOR_SPI0_CS0_GPIO, false);
    stub_gpio_set_level(SPI_MONITOR_SPI0_CS1_GPIO, false);
    stub_time_us32 = 100u;
    spi_monitor_poll();

    stub_gpio_set_level(SPI_MONITOR_SPI0_CS0_GPIO, true);
    stub_gpio_set_level(SPI_MONITOR_SPI0_CS1_GPIO, true);
    stub_gpio_fire_irq(SPI_MONITOR_SPI0_CS0_GPIO, GPIO_IRQ_EDGE_RISE);
    stub_gpio_fire_irq(SPI_MONITOR_SPI0_CS1_GPIO, GPIO_IRQ_EDGE_RISE);
    stub_time_us32 = 2000u;
    spi_monitor_poll();
    assert(trace_ring_available() == 0u);

    stub_time_us32 = 2100u;
    spi_monitor_poll();

    assert(trace_ring_available() == 2u);
    assert(trace_ring_pop_copy(&packet) == true);
    assert(packet.header.channel == SPI_MONITOR_CH0_LOGICAL_CHANNEL);
    assert((packet.header.flags & TRACE_FLAG_END) != 0u);
    assert(packet.header.payload_len == 1u);
    assert(packet.payload[0] == 0x11u);

    assert(trace_ring_pop_copy(&packet) == true);
    assert(packet.header.channel == SPI_MONITOR_CH1_LOGICAL_CHANNEL);
    assert((packet.header.flags & TRACE_FLAG_END) != 0u);
    assert(packet.header.payload_len == 1u);
    assert(packet.payload[0] == 0x22u);
}

/** @brief Verify that sampler overruns remain visible on both the owning channel and the bus aggregate. */
static void test_spi_monitor_directional_overruns_are_aggregated_in_status(void) {
    spi_monitor_bus_config_t config = {0};
    spi_monitor_bus_status_t bus_status;
    spi_monitor_channel_status_t status[SPI_MONITOR_CHANNEL_COUNT];

    reset_real_spi_monitor_state();
    config.capture = SPI_MONITOR_CAPTURE_MOSI_MISO;
    config.spi_mode = 0u;
    config.channel_select_mask = 0x02u;
    config.timeout_us = 1000u;

    assert(spi_monitor_set_bus_config(0u, &config) == SPI_MONITOR_RC_OK);
    spi_monitor_test_set_bus_sampler_overrun_counts(0u, 2u, 3u);

    assert(spi_monitor_get_bus_status(0u, &bus_status) == SPI_MONITOR_RC_OK);
    assert(spi_monitor_get_all_status(status) == SPI_MONITOR_RC_OK);
    assert(bus_status.overrun_count == 5u);
    assert(status[1].overrun_count == 5u);
}

/** @brief Verify that timeout-driven runtime flushes refresh per-channel overrun status immediately. */
static void test_spi_monitor_poll_timeout_refreshes_channel_overrun_status(void) {
    spi_monitor_bus_config_t config = {0};
    spi_monitor_channel_status_t status[SPI_MONITOR_CHANNEL_COUNT];
    trace_packet_t packet = make_test_trace_packet(1u);
    uint32_t raw_words[1];
    uint32_t index;

    reset_real_spi_monitor_state();
    trace_ring_init();
    config.capture = SPI_MONITOR_CAPTURE_MOSI;
    config.spi_mode = 0u;
    config.channel_select_mask = 0x01u;
    config.timeout_us = 1000u;
    raw_words[0] = pack_spi_mosi_word(0xAAu);

    for (index = 0u; index < TRACE_RING_CAPACITY; ++index) {
        packet.header.sequence = (uint16_t)index;
        assert(trace_ring_push(&packet) == true);
    }

    assert(spi_monitor_set_bus_config(0u, &config) == SPI_MONITOR_RC_OK);
    assert(spi_monitor_test_feed_samples(0u, 0x01u, 25u, raw_words, 1u) == true);
    assert(spi_monitor_get_all_status(status) == SPI_MONITOR_RC_OK);
    assert(status[0].overrun_count == 0u);

    stub_time_us32 = 2000u;
    spi_monitor_poll();
    assert(spi_monitor_get_all_status(status) == SPI_MONITOR_RC_OK);
    assert(status[0].overrun_count == 0u);

    stub_time_us32 = 2100u;
    spi_monitor_poll();

    assert(spi_monitor_get_all_status(status) == SPI_MONITOR_RC_OK);
    assert(status[0].overrun_count == 1u);
}

/** @brief Verify that timeout polling ignores stale timestamps older than the most recent SPI activity. */
static void test_spi_monitor_poll_timeout_ignores_stale_now_snapshot(void) {
    spi_monitor_bus_config_t config = {0};
    trace_packet_t packet = {0};
    uint32_t raw_words[1];

    reset_real_spi_monitor_state();
    trace_ring_init();
    config.capture = SPI_MONITOR_CAPTURE_MOSI;
    config.spi_mode = 0u;
    config.channel_select_mask = 0x01u;
    config.timeout_us = 1000u;
    raw_words[0] = pack_spi_mosi_word(0x6Au);

    assert(spi_monitor_set_bus_config(0u, &config) == SPI_MONITOR_RC_OK);
    assert(spi_monitor_test_feed_samples(0u, 0x01u, 100u, raw_words, 1u) == true);

    spi_monitor_test_poll_timeout(0u, 90u);

    assert(trace_ring_available() == 0u);

    spi_monitor_test_poll_timeout(0u, 1200u);
    assert(trace_ring_available() == 0u);
    spi_monitor_test_poll_timeout(0u, 1300u);

    assert(trace_ring_available() == 1u);
    assert(trace_ring_pop_copy(&packet) == true);
    assert((packet.header.flags & TRACE_FLAG_END) != 0u);
    assert(packet.header.sequence == 1u);
    assert(packet.header.payload_len == 1u);
    assert(packet.payload[0] == 0x6Au);
}

/** @brief Verify that a latched sampler boundary offset splits same-channel transactions before the next poll. */
static void test_spi_monitor_boundary_offset_splits_same_channel_transactions(void) {
    spi_monitor_bus_config_t config = {0};
    trace_packet_t packet = {0};
    uint32_t raw_words[2];

    reset_real_spi_monitor_state();
    trace_ring_init();
    config.capture = SPI_MONITOR_CAPTURE_MOSI;
    config.spi_mode = 0u;
    config.channel_select_mask = 0x01u;
    config.timeout_us = 1000u;
    raw_words[0] = pack_spi_mosi_word(0x12u);
    raw_words[1] = pack_spi_mosi_word(0x34u);

    assert(spi_monitor_set_bus_config(0u, &config) == SPI_MONITOR_RC_OK);
    assert(spi_monitor_test_stage_channel_dma_progress_with_boundary(0u, raw_words, 2u, 1u) == true);

    stub_gpio_set_level(SPI_MONITOR_SPI0_CS0_GPIO, false);
    stub_time_us32 = 100u;
    spi_monitor_poll();

    assert(trace_ring_available() == 1u);
    assert(trace_ring_pop_copy(&packet) == true);
    assert(packet.header.channel == SPI_MONITOR_CH0_LOGICAL_CHANNEL);
    assert((packet.header.flags & TRACE_FLAG_END) != 0u);
    assert(packet.header.sequence == 1u);
    assert(packet.header.payload_len == 1u);
    assert(packet.payload[0] == 0x12u);

    stub_gpio_set_level(SPI_MONITOR_SPI0_CS0_GPIO, true);
    stub_gpio_fire_irq(SPI_MONITOR_SPI0_CS0_GPIO, GPIO_IRQ_EDGE_RISE);
    stub_gpio_set_level(SPI_MONITOR_SPI0_CS0_GPIO, false);
    spi_monitor_test_poll_timeout(0u, 1200u);
    assert(trace_ring_available() == 0u);
    spi_monitor_test_poll_timeout(0u, 1300u);

    assert(trace_ring_available() == 1u);
    assert(trace_ring_pop_copy(&packet) == true);
    assert(packet.header.channel == SPI_MONITOR_CH0_LOGICAL_CHANNEL);
    assert((packet.header.flags & TRACE_FLAG_END) != 0u);
    assert(packet.header.sequence == 2u);
    assert(packet.header.payload_len == 1u);
    assert(packet.payload[0] == 0x34u);
}

/** @brief Verify that a fragment emitted after mid-transaction overflow still carries CONTINUED while saturated backlog stays dropped. */
static void test_spi_monitor_overflow_preserves_continued_for_same_transaction(void) {
    spi_monitor_bus_config_t config = {0};
    trace_packet_t filler = make_test_trace_packet(1u);
    trace_packet_t packet = {0};
    uint32_t raw_word[1];
    uint32_t index;

    reset_real_spi_monitor_state();
    trace_ring_init();
    config.capture = SPI_MONITOR_CAPTURE_MOSI;
    config.spi_mode = 0u;
    config.channel_select_mask = 0x01u;
    config.timeout_us = 1000u;

    assert(spi_monitor_set_bus_config(0u, &config) == SPI_MONITOR_RC_OK);

    for (index = 0u; index < (TRACE_PACKET_PAYLOAD_BYTES + 1u); ++index) {
        raw_word[0] = pack_spi_mosi_word((uint8_t)index);
        assert(spi_monitor_test_feed_samples(0u, 0x01u, 10u, raw_word, 1u) == true);
    }

    assert(trace_ring_available() == 1u);

    for (index = 0u; index < (TRACE_RING_CAPACITY - 1u); ++index) {
        filler.header.sequence = (uint16_t)index;
        assert(trace_ring_push(&filler) == true);
    }

    for (index = 0u; index < TRACE_PACKET_PAYLOAD_BYTES; ++index) {
        raw_word[0] = pack_spi_mosi_word((uint8_t)(0x40u + index));
        assert(spi_monitor_test_feed_samples(0u, 0x01u, 20u, raw_word, 1u) == true);
    }

    raw_word[0] = pack_spi_mosi_word(0xE1u);
    assert(spi_monitor_test_feed_samples(0u, 0x01u, 30u, raw_word, 1u) == true);

    while (trace_ring_available() != 0u) {
        assert(trace_ring_pop_copy(&packet) == true);
    }

    assert(spi_monitor_test_feed_samples(0u, 0x01u, 40u, raw_word, 1u) == true);
    spi_monitor_test_poll_timeout(0u, 2000u);
    assert(trace_ring_available() == 0u);
    spi_monitor_test_poll_timeout(0u, 2100u);

    assert(trace_ring_available() == 1u);
    assert(trace_ring_pop_copy(&packet) == true);
    assert((packet.header.flags & TRACE_FLAG_OVERFLOW) != 0u);
    assert((packet.header.flags & TRACE_FLAG_CONTINUED) != 0u);
    assert((packet.header.flags & TRACE_FLAG_END) != 0u);
    assert(packet.header.sequence == 1u);
    assert(packet.header.payload_len == 1u);
    assert(packet.payload[0] == 0xE1u);
}

/** @brief Verify that an extra pending boundary keeps the earliest split without surfacing as a sampler overrun. */
static void test_spi_monitor_pending_boundary_keeps_earliest_split_without_overrun(void) {
    spi_monitor_bus_config_t config = {0};
    spi_monitor_channel_status_t status[SPI_MONITOR_CHANNEL_COUNT];
    trace_packet_t packet = {0};
    uint32_t raw_words[3];

    reset_real_spi_monitor_state();
    trace_ring_init();
    config.capture = SPI_MONITOR_CAPTURE_MOSI;
    config.spi_mode = 0u;
    config.channel_select_mask = 0x01u;
    config.timeout_us = 1000u;
    raw_words[0] = pack_spi_mosi_word(0x12u);
    raw_words[1] = pack_spi_mosi_word(0x34u);
    raw_words[2] = pack_spi_mosi_word(0x56u);

    assert(spi_monitor_set_bus_config(0u, &config) == SPI_MONITOR_RC_OK);
    assert(spi_monitor_test_stage_channel_dma_progress_with_boundary(0u, raw_words, 3u, 1u) == true);

    stub_gpio_set_level(SPI_MONITOR_SPI0_CS0_GPIO, true);
    stub_gpio_fire_irq(SPI_MONITOR_SPI0_CS0_GPIO, GPIO_IRQ_EDGE_RISE);
    stub_gpio_set_level(SPI_MONITOR_SPI0_CS0_GPIO, false);
    stub_time_us32 = 100u;
    spi_monitor_poll();

    assert(spi_monitor_get_all_status(status) == SPI_MONITOR_RC_OK);
    assert(status[0].overrun_count == 0u);
    assert(trace_ring_available() == 1u);
    assert(trace_ring_pop_copy(&packet) == true);
    assert(packet.header.sequence == 1u);
    assert(packet.header.payload_len == 1u);
    assert(packet.payload[0] == 0x12u);

    spi_monitor_test_poll_timeout(0u, 1200u);
    assert(trace_ring_available() == 0u);
    spi_monitor_test_poll_timeout(0u, 1300u);

    assert(trace_ring_available() == 1u);
    assert(trace_ring_pop_copy(&packet) == true);
    assert(packet.header.sequence == 2u);
    assert(packet.header.payload_len == 2u);
    assert(packet.payload[0] == 0x34u);
    assert(packet.payload[1] == 0x56u);
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

static void test_cdc_rx_callback_leaves_remaining_packets_for_later_pass(void) {
    uint8_t payload[192];
    uint8_t drained[192];

    reset_usb_stub();
    for (uint32_t index = 0u; index < sizeof(payload); ++index) {
        payload[index] = (uint8_t)index;
    }
    load_cdc_rx(payload, sizeof(payload));

    tud_cdc_rx_cb(0u);

    assert(stub_cdc_rx_offset == 128u);
    assert(usb_cdc_read(drained, sizeof(drained)) == 128u);
    assert(memcmp(drained, payload, 128u) == 0);

    tud_cdc_rx_cb(0u);

    assert(stub_cdc_rx_offset == sizeof(payload));
    assert(usb_cdc_read(drained, sizeof(drained)) == 64u);
    assert(memcmp(drained, &payload[128], 64u) == 0);
}

static void test_cdc_rx_callback_preserves_packet_tail_when_queue_fills(void) {
    uint8_t initial[512];
    uint8_t overflow[80];
    uint8_t drained[512];
    usb_cdc_stats_t stats;

    reset_usb_stub();
    while (usb_cdc_read(drained, sizeof(drained)) != 0u) {
    }
    for (uint32_t index = 0u; index < sizeof(initial); ++index) {
        initial[index] = (uint8_t)index;
    }
    for (uint32_t index = 0u; index < sizeof(overflow); ++index) {
        overflow[index] = (uint8_t)(0x80u + index);
    }

    load_cdc_rx(initial, 256u);
    tud_cdc_rx_cb(0u);
    tud_cdc_rx_cb(0u);
    load_cdc_rx(&initial[256], 256u);
    tud_cdc_rx_cb(0u);
    tud_cdc_rx_cb(0u);

    load_cdc_rx(overflow, sizeof(overflow));
    tud_cdc_rx_cb(0u);

    assert(stub_cdc_rx_offset == 0u);
    stats = usb_cdc_get_stats();
    assert(stats.rx_dropped_bytes == 0u);
    assert(usb_cdc_read(drained, sizeof(drained)) == sizeof(initial));
    assert(memcmp(drained, initial, sizeof(initial)) == 0);

    tud_cdc_rx_cb(0u);

    assert(stub_cdc_rx_offset == sizeof(overflow));
    assert(usb_cdc_read(drained, sizeof(drained)) == sizeof(overflow));
    assert(memcmp(drained, overflow, sizeof(overflow)) == 0);
}

static void test_cdc_tx_poll_limits_one_pass_budget(void) {
    uint8_t payload[192];
    uint8_t drained[512];

    reset_usb_stub();
    while (usb_cdc_read(drained, sizeof(drained)) != 0u) {
    }
    usb_cdc_poll_tx();
    stub_cdc_tx_length = 0u;
    stub_cdc_flush_calls = 0u;
    memset(stub_cdc_tx_data, 0, sizeof(stub_cdc_tx_data));
    for (uint32_t index = 0u; index < sizeof(payload); ++index) {
        payload[index] = (uint8_t)(0xA0u + index);
    }

    assert(usb_cdc_write(payload, sizeof(payload)) == true);
    assert(stub_cdc_tx_length == 128u);
    assert(memcmp(stub_cdc_tx_data, payload, 128u) == 0);

    usb_cdc_poll_tx();

    assert(stub_cdc_tx_length == sizeof(payload));
    assert(memcmp(stub_cdc_tx_data, payload, sizeof(payload)) == 0);
}

static void test_cdc_tx_enqueue_failure_is_counted(void) {
    uint8_t payload[1025u];
    usb_cdc_stats_t stats;

    reset_usb_stub();
    memset(payload, 0x5Au, sizeof(payload));

    assert(usb_cdc_write(payload, sizeof(payload)) == false);
    stats = usb_cdc_get_stats();
    assert(stats.tx_enqueue_failures == 1u);
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

static void test_cli_spimon_command_updates_monitor_bus(void) {
    static const uint8_t payload[] = {'s','p','i','m','o','n',' ','1',' ','b','o','t','h',' ','a','l','l',' ','3',' ','2','5','0','0','\r'};

    reset_usb_stub();
    load_cdc_rx(payload, sizeof(payload));

    tud_cdc_rx_cb(0u);
    device_cli_poll();

    assert(stub_spi_running[2] == true);
    assert(stub_spi_running[3] == true);
    assert(stub_spi_channel_select_mask[1] == SPI_MONITOR_CHANNEL_SELECT_ALL);
    assert(stub_spi_capture[2] == SPI_MONITOR_CAPTURE_MOSI_MISO);
    assert(stub_spi_capture[3] == SPI_MONITOR_CAPTURE_MOSI_MISO);
    assert(stub_spi_mode[2] == 3u);
    assert(stub_spi_mode[3] == 3u);
    assert(stub_spi_timeout_us[2] == 2500u);
    assert(stub_spi_timeout_us[3] == 2500u);
    assert(strstr((const char *)stub_cdc_tx_data, "spimon bus1 running select=all capture=both mode=3 timeout_us=2500") != NULL);
}

static void test_cli_spimon_command_updates_one_selected_channel(void) {
    static const uint8_t payload[] = {'s','p','i','m','o','n',' ','0',' ','m','o','s','i',' ','1',' ','1',' ','1','5','0','0','\r'};

    reset_usb_stub();
    load_cdc_rx(payload, sizeof(payload));

    tud_cdc_rx_cb(0u);
    device_cli_poll();

    assert(stub_spi_running[0] == false);
    assert(stub_spi_running[1] == true);
    assert(stub_spi_channel_select_mask[0] == 0x02u);
    assert(strstr((const char *)stub_cdc_tx_data, "spimon bus0 running select=ch1 capture=mosi mode=1 timeout_us=1500") != NULL);
}

static void test_cli_spimon_status_reports_bus_state(void) {
    static const uint8_t payload[] = {'s','p','i','m','o','n',' ','s','t','a','t','u','s',' ','0','\r'};

    reset_usb_stub();
    stub_spi_bus_running[0] = true;
    stub_spi_bus_capture[0] = SPI_MONITOR_CAPTURE_MOSI;
    stub_spi_bus_mode[0] = 0u;
    stub_spi_bus_timeout_us[0] = 1200u;
    stub_spi_channel_select_mask[0] = 0x01u;
    stub_spi_running[0] = true;
    stub_spi_running[1] = false;
    stub_spi_running[2] = false;
    stub_spi_running[3] = false;
    stub_spi_capture[0] = SPI_MONITOR_CAPTURE_MOSI;
    stub_spi_mode[0] = 0u;
    stub_spi_timeout_us[0] = 1200u;
    stub_spi_packets_emitted[0] = 9u;
    stub_spi_transactions_emitted[0] = 3u;
    stub_spi_overrun_count[0] = 2u;
    stub_spi_timeout_close_count[0] = 1u;
    load_cdc_rx(payload, sizeof(payload));

    tud_cdc_rx_cb(0u);
    device_cli_poll();

    assert(strstr((const char *)stub_cdc_tx_data, "spimon bus0 running select=ch0 capture=mosi mode=0 timeout_us=1200 packets=9 txns=3 overruns=2 timeout_closes=1") != NULL);
}

static void test_cli_spimon_reports_disabled_state(void) {
    static const uint8_t payload[] = {'s','p','i','m','o','n',' ','1',' ','m','o','s','i',' ','a','l','l',' ','1','\r'};

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

static void test_hid_second_report_is_rejected_without_overwriting_pending_command(void) {
    usb_hid_command_t first = {0};
    usb_hid_command_t second = {0};
    usb_hid_command_t response = {0};
    usb_hid_stats_t stats;

    reset_usb_stub();
    first.opcode = USB_HID_OPCODE_LED_ON;
    first.sequence = 10u;
    second.opcode = USB_HID_OPCODE_LED_OFF;
    second.sequence = 11u;

    tud_hid_set_report_cb(0u, 0u, HID_REPORT_TYPE_OUTPUT, (uint8_t const *)&first, sizeof(first));
    tud_hid_set_report_cb(0u, 0u, HID_REPORT_TYPE_OUTPUT, (uint8_t const *)&second, sizeof(second));

    assert(stub_led_set_calls == 0u);
    stats = usb_hid_get_stats();
    assert(stats.busy_rejects == 1u);
    assert(tud_hid_get_report_cb(0u, 0u, HID_REPORT_TYPE_INPUT, (uint8_t *)&response, sizeof(response)) == sizeof(response));
    assert(response.opcode == USB_HID_OPCODE_LED_OFF);
    assert(response.sequence == 11u);
    assert(response.status == USB_HID_STATUS_BUSY);

    usb_hid_poll();

    assert(stub_led_set_calls == 1u);
    assert(stub_led_state == true);
    assert(tud_hid_get_report_cb(0u, 0u, HID_REPORT_TYPE_INPUT, (uint8_t *)&response, sizeof(response)) == sizeof(response));
    assert(response.opcode == USB_HID_OPCODE_LED_ON);
    assert(response.sequence == 10u);
    assert(response.status == USB_HID_STATUS_OK);
}

static void test_hid_unknown_opcode_is_counted(void) {
    usb_hid_command_t command = {0};
    usb_hid_command_t response = {0};
    usb_hid_stats_t stats;

    reset_usb_stub();
    command.opcode = 0x7Fu;
    command.sequence = 20u;

    tud_hid_set_report_cb(0u, 0u, HID_REPORT_TYPE_OUTPUT, (uint8_t const *)&command, sizeof(command));
    usb_hid_poll();

    stats = usb_hid_get_stats();
    assert(stats.unknown_opcodes == 1u);
    assert(tud_hid_get_report_cb(0u, 0u, HID_REPORT_TYPE_INPUT, (uint8_t *)&response, sizeof(response)) == sizeof(response));
    assert(response.opcode == 0x7Fu);
    assert(response.status == USB_HID_STATUS_UNKNOWN_COMMAND);
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
    usb_bulk_service_stream(app_control_stream_enabled());

    assert(stub_vendor_tx_length == packet_bytes);
    assert(stub_vendor_write_calls == 1u);
    assert(memcmp(stub_vendor_tx_data, &packet, packet_bytes) == 0);
    assert(trace_ring_available() == 0u);
    assert(stub_vendor_flush_calls == 0u);
}

static void test_usb_bulk_service_stream_counts_host_backpressure_stalls(void) {
    trace_packet_t packet = make_test_trace_packet(45u);
    uint32_t host_stalls_before;

    reset_usb_stub();
    trace_ring_init();

    host_stalls_before = usb_bulk_host_backpressure_stall_count();
    assert(trace_ring_push(&packet) == true);
    stub_vendor_available = 0u;

    assert(usb_bulk_service_stream(app_control_stream_enabled()) == false);
    assert(usb_bulk_host_backpressure_stall_count() == (host_stalls_before + 1u));
}

static void test_usb_bulk_service_stream_uses_all_available_vendor_space(void) {
    trace_packet_t packet = make_test_trace_packet(46u);
    uint32_t packet_bytes;

    reset_usb_stub();
    trace_ring_init();
    packet.header.payload_len = 114u;
    for (uint32_t index = 0u; index < packet.header.payload_len; ++index) {
        packet.payload[index] = (uint8_t)(0x40u + index);
    }
    packet_bytes = TRACE_PACKET_HEADER_BYTES + packet.header.payload_len;
    stub_vendor_available = 100u;
    assert(trace_ring_push(&packet) == true);

    assert(usb_bulk_service_stream(app_control_stream_enabled()) == true);
    assert(stub_vendor_tx_length == 100u);
    assert(trace_ring_available() == 1u);
    assert(packet_bytes > stub_vendor_tx_length);
    assert(memcmp(stub_vendor_tx_data, &packet, stub_vendor_tx_length) == 0);
}

static void test_usb_bulk_poll_stream_resumes_partial_trace_packet_write(void) {
    trace_packet_t packet = make_test_trace_packet(41u);
    uint32_t packet_bytes = TRACE_PACKET_HEADER_BYTES + packet.header.payload_len;

    reset_usb_stub();
    trace_ring_init();

    assert(trace_ring_push(&packet) == true);
    stub_vendor_available = 8u;
    usb_bulk_service_stream(app_control_stream_enabled());
    assert(stub_vendor_tx_length == 8u);
    assert(trace_ring_available() == 1u);

    stub_vendor_available = sizeof(stub_vendor_tx_data) - stub_vendor_tx_length;
    usb_bulk_service_stream(app_control_stream_enabled());

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
    usb_bulk_service_stream(app_control_stream_enabled());
    assert(stub_vendor_tx_length == 8u);
    assert(trace_ring_available() == 1u);

    app_control_set_stream_enabled(false);
    usb_bulk_service_stream(app_control_stream_enabled());

    memset(stub_vendor_tx_data, 0, sizeof(stub_vendor_tx_data));
    stub_vendor_tx_length = 0u;
    stub_vendor_available = sizeof(stub_vendor_tx_data);
    app_control_set_stream_enabled(true);
    usb_bulk_service_stream(app_control_stream_enabled());

    assert(stub_vendor_tx_length == packet_bytes);
    assert(memcmp(stub_vendor_tx_data, &packet, packet_bytes) == 0);
    assert(trace_ring_available() == 0u);
}

static void test_usb_bulk_poll_stream_clamps_invalid_payload_len(void) {
    trace_packet_t packet = make_test_trace_packet(44u);
    uint32_t packet_bytes;

    reset_usb_stub();
    trace_ring_init();

    packet.header.payload_len = (uint16_t)(TRACE_PACKET_PAYLOAD_BYTES + 1u);
    assert(trace_ring_push(&packet) == true);
    usb_bulk_service_stream(app_control_stream_enabled());

    packet_bytes = TRACE_PACKET_HEADER_BYTES + TRACE_PACKET_PAYLOAD_BYTES;
    assert(stub_vendor_tx_length == packet_bytes);
    assert(trace_ring_available() == 0u);
    assert(trace_ring_total_consumed() == 1u);
}

static void test_usb_bulk_poll_stream_writes_unaligned_whole_packet_tail(void) {
    trace_packet_t packet = make_test_trace_packet(43u);
    uint32_t packet_bytes;

    reset_usb_stub();
    trace_ring_init();

    packet.header.payload_len = 80u;
    for (uint32_t index = 0u; index < packet.header.payload_len; ++index) {
        packet.payload[index] = (uint8_t)(0x20u + index);
    }
    packet_bytes = TRACE_PACKET_HEADER_BYTES + packet.header.payload_len;

    assert(trace_ring_push(&packet) == true);
    usb_bulk_service_stream(app_control_stream_enabled());

    assert(stub_vendor_tx_length == packet_bytes);
    assert(stub_vendor_write_calls == 1u);
    assert(memcmp(stub_vendor_tx_data, &packet, packet_bytes) == 0);
}

static void test_usb_bulk_poll_stream_emits_nothing_when_ring_empty(void) {
    reset_usb_stub();
    usb_bulk_service_stream(app_control_stream_enabled());

    assert(stub_vendor_tx_length == 0u);
    assert(stub_vendor_flush_calls == 0u);
}

static void test_usb_bulk_service_stream_uses_partial_vendor_space(void) {
    trace_packet_t packet = make_test_trace_packet(47u);

    reset_usb_stub();
    trace_ring_init();

    packet.header.payload_len = 600u;
    for (uint32_t index = 0u; index < packet.header.payload_len; ++index) {
        packet.payload[index] = (uint8_t)(index & 0xFFu);
    }

    assert(trace_ring_push(&packet) == true);
    stub_vendor_available = 512u;

    assert(usb_bulk_service_stream(app_control_stream_enabled()) == true);
    assert(stub_vendor_tx_length == 512u);
    assert(memcmp(stub_vendor_tx_data, &packet, stub_vendor_tx_length) == 0);
    assert(stub_vendor_available == 0u);
    assert(trace_ring_available() == 1u);
}

static void test_usb_bulk_flush_flushes_vendor_endpoint(void) {
    reset_usb_stub();
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
    assert(response.payload_length == 6u);
    assert(response.payload[0] == 1u);
    assert(response.payload[1] == 4u);
    assert(memcmp(&response.payload[2], "test", 4u) == 0);
}

static void test_cli_version_reports_firmware_version(void) {
    static const uint8_t payload[] = {'v', 'e', 'r', 's', 'i', 'o', 'n', '\r'};

    reset_usb_stub();
    load_cdc_rx(payload, sizeof(payload));

    tud_cdc_rx_cb(0u);
    device_cli_poll();

    assert(strstr((const char *)stub_cdc_tx_data, "firmware_version=test") != NULL);
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

static void test_hid_i2c_monitor_get_status_reports_invalid_channel(void) {
    usb_hid_command_t command = {0};
    usb_hid_command_t response = {0};

    reset_usb_stub();
    command.opcode = USB_HID_OPCODE_I2C_MONITOR_GET_STATUS;
    command.sequence = 16u;
    command.payload_length = 1u;
    command.payload[0] = 7u;

    tud_hid_set_report_cb(0u, 0u, HID_REPORT_TYPE_OUTPUT, (uint8_t const *)&command, sizeof(command));
    usb_hid_poll();

    assert(tud_hid_get_report_cb(0u, 0u, HID_REPORT_TYPE_INPUT, (uint8_t *)&response, sizeof(response)) == sizeof(response));
    assert(response.opcode == USB_HID_OPCODE_I2C_MONITOR_GET_STATUS);
    assert(response.status == USB_HID_STATUS_BAD_LENGTH);
    assert(response.payload_length == 0u);
}

static void test_hid_i2c_monitor_get_all_status_reports_busy(void) {
    usb_hid_command_t command = {0};
    usb_hid_command_t response = {0};

    reset_usb_stub();
    stub_monitor_result = I2C_MONITOR_RC_BUSY;
    command.opcode = USB_HID_OPCODE_I2C_MONITOR_GET_ALL_STATUS;
    command.sequence = 17u;

    tud_hid_set_report_cb(0u, 0u, HID_REPORT_TYPE_OUTPUT, (uint8_t const *)&command, sizeof(command));
    usb_hid_poll();

    assert(tud_hid_get_report_cb(0u, 0u, HID_REPORT_TYPE_INPUT, (uint8_t *)&response, sizeof(response)) == sizeof(response));
    assert(response.opcode == USB_HID_OPCODE_I2C_MONITOR_GET_ALL_STATUS);
    assert(response.status == USB_HID_STATUS_BUSY);
    assert(response.payload_length == 0u);
}

static void test_hid_spi_monitor_set_config_updates_bus(void) {
    usb_hid_command_t command = {0};
    usb_hid_command_t response = {0};

    reset_usb_stub();
    command.opcode = USB_HID_OPCODE_SPI_MONITOR_SET_CONFIG;
    command.sequence = 11u;
    command.payload_length = 8u;
    command.payload[0] = 1u;
    command.payload[1] = SPI_MONITOR_CAPTURE_MOSI_MISO;
    command.payload[2] = 3u;
    command.payload[3] = SPI_MONITOR_CHANNEL_SELECT_ALL;
    command.payload[4] = 0xC4u;
    command.payload[5] = 0x09u;
    command.payload[6] = 0x00u;
    command.payload[7] = 0x00u;

    tud_hid_set_report_cb(0u, 0u, HID_REPORT_TYPE_OUTPUT, (uint8_t const *)&command, sizeof(command));
    usb_hid_poll();

    assert(stub_spi_running[2] == true);
    assert(stub_spi_running[3] == true);
    assert(stub_spi_channel_select_mask[1] == SPI_MONITOR_CHANNEL_SELECT_ALL);
    assert(stub_spi_capture[2] == SPI_MONITOR_CAPTURE_MOSI_MISO);
    assert(stub_spi_capture[3] == SPI_MONITOR_CAPTURE_MOSI_MISO);
    assert(stub_spi_mode[2] == 3u);
    assert(stub_spi_mode[3] == 3u);
    assert(stub_spi_timeout_us[2] == 2500u);
    assert(stub_spi_timeout_us[3] == 2500u);
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
    command.payload_length = 8u;
    command.payload[0] = 0u;
    command.payload[1] = SPI_MONITOR_CAPTURE_MOSI;
    command.payload[2] = 1u;
    command.payload[3] = 0x01u;
    command.payload[4] = 0x00u;
    command.payload[5] = 0x00u;
    command.payload[6] = 0x00u;
    command.payload[7] = 0x00u;

    tud_hid_set_report_cb(0u, 0u, HID_REPORT_TYPE_OUTPUT, (uint8_t const *)&command, sizeof(command));
    usb_hid_poll();

    assert(tud_hid_get_report_cb(0u, 0u, HID_REPORT_TYPE_INPUT, (uint8_t *)&response, sizeof(response)) == sizeof(response));
    assert(response.opcode == USB_HID_OPCODE_SPI_MONITOR_SET_CONFIG);
    assert(response.status == USB_HID_STATUS_BUSY);
}

static void test_hid_spi_monitor_get_status_returns_bus_payload(void) {
    usb_hid_command_t command = {0};
    usb_hid_command_t response = {0};

    reset_usb_stub();
    stub_spi_bus_running[1] = true;
    stub_spi_bus_capture[1] = SPI_MONITOR_CAPTURE_MOSI;
    stub_spi_bus_mode[1] = 2u;
    stub_spi_bus_timeout_us[1] = 1800u;
    stub_spi_channel_select_mask[1] = 0x01u;
    stub_spi_running[2] = true;
    stub_spi_running[3] = false;
    stub_spi_capture[2] = SPI_MONITOR_CAPTURE_MOSI;
    stub_spi_mode[2] = 2u;
    stub_spi_timeout_us[2] = 1800u;
    stub_spi_packets_emitted[2] = 7u;
    stub_spi_transactions_emitted[2] = 2u;
    stub_spi_overrun_count[2] = 3u;
    stub_spi_timeout_close_count[1] = 4u;
    command.opcode = USB_HID_OPCODE_SPI_MONITOR_GET_STATUS;
    command.sequence = 13u;
    command.payload_length = 1u;
    command.payload[0] = 1u;

    tud_hid_set_report_cb(0u, 0u, HID_REPORT_TYPE_OUTPUT, (uint8_t const *)&command, sizeof(command));
    usb_hid_poll();

    assert(tud_hid_get_report_cb(0u, 0u, HID_REPORT_TYPE_INPUT, (uint8_t *)&response, sizeof(response)) == sizeof(response));
    assert(response.opcode == USB_HID_OPCODE_SPI_MONITOR_GET_STATUS);
    assert(response.status == USB_HID_STATUS_OK);
    assert(response.payload_length == 46u);
    assert(response.payload[0] == 1u);
    assert(response.payload[1] == 1u);
    assert(response.payload[2] == 1u);
    assert(response.payload[3] == SPI_MONITOR_CAPTURE_MOSI);
    assert(response.payload[4] == 2u);
    assert(response.payload[5] == 0x01u);
    assert(response.payload[6] == 0x08u);
    assert(response.payload[7] == 0x07u);
    assert(response.payload[8] == 0x00u);
    assert(response.payload[10] == 7u);
    assert(response.payload[18] == 3u);
    assert(response.payload[22] == 0u);
    assert(response.payload[26] == 0u);
    assert(response.payload[30] == 0u);
    assert(response.payload[34] == 0u);
    assert(response.payload[38] == 0u);
    assert(response.payload[42] == 4u);
    assert(response.payload[43] == 0u);
    assert(response.payload[44] == 0u);
    assert(response.payload[45] == 0u);
}

static void test_hid_spi_monitor_get_all_status_returns_all_channels(void) {
    usb_hid_command_t command = {0};
    usb_hid_command_t response = {0};

    reset_usb_stub();
    stub_spi_running[0] = true;
    stub_spi_running[1] = true;
    stub_spi_running[2] = true;
    stub_spi_running[3] = true;
    stub_spi_capture[0] = SPI_MONITOR_CAPTURE_MOSI;
    stub_spi_capture[1] = SPI_MONITOR_CAPTURE_MOSI;
    stub_spi_capture[2] = SPI_MONITOR_CAPTURE_MOSI_MISO;
    stub_spi_capture[3] = SPI_MONITOR_CAPTURE_MOSI_MISO;
    stub_spi_mode[0] = 0u;
    stub_spi_mode[1] = 0u;
    stub_spi_mode[2] = 3u;
    stub_spi_mode[3] = 3u;
    stub_spi_timeout_us[0] = 1000u;
    stub_spi_timeout_us[1] = 1000u;
    stub_spi_timeout_us[2] = 2500u;
    stub_spi_timeout_us[3] = 2500u;
    stub_spi_overrun_count[2] = 1u;
    command.opcode = USB_HID_OPCODE_SPI_MONITOR_GET_ALL_STATUS;
    command.sequence = 14u;

    tud_hid_set_report_cb(0u, 0u, HID_REPORT_TYPE_OUTPUT, (uint8_t const *)&command, sizeof(command));
    usb_hid_poll();

    assert(tud_hid_get_report_cb(0u, 0u, HID_REPORT_TYPE_INPUT, (uint8_t *)&response, sizeof(response)) == sizeof(response));
    assert(response.opcode == USB_HID_OPCODE_SPI_MONITOR_GET_ALL_STATUS);
    assert(response.status == USB_HID_STATUS_OK);
    assert(response.payload_length == 40u);
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
    assert(response.payload[39] == 0u);
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

static void test_hid_spi_monitor_get_status_reports_invalid_bus(void) {
    usb_hid_command_t command = {0};
    usb_hid_command_t response = {0};

    reset_usb_stub();
    command.opcode = USB_HID_OPCODE_SPI_MONITOR_GET_STATUS;
    command.sequence = 18u;
    command.payload_length = 1u;
    command.payload[0] = 3u;

    tud_hid_set_report_cb(0u, 0u, HID_REPORT_TYPE_OUTPUT, (uint8_t const *)&command, sizeof(command));
    usb_hid_poll();

    assert(tud_hid_get_report_cb(0u, 0u, HID_REPORT_TYPE_INPUT, (uint8_t *)&response, sizeof(response)) == sizeof(response));
    assert(response.opcode == USB_HID_OPCODE_SPI_MONITOR_GET_STATUS);
    assert(response.status == USB_HID_STATUS_BAD_LENGTH);
    assert(response.payload_length == 0u);
}

static void test_hid_spi_monitor_get_all_status_reports_busy(void) {
    usb_hid_command_t command = {0};
    usb_hid_command_t response = {0};

    reset_usb_stub();
    stub_spi_monitor_result = SPI_MONITOR_RC_BUSY;
    command.opcode = USB_HID_OPCODE_SPI_MONITOR_GET_ALL_STATUS;
    command.sequence = 19u;

    tud_hid_set_report_cb(0u, 0u, HID_REPORT_TYPE_OUTPUT, (uint8_t const *)&command, sizeof(command));
    usb_hid_poll();

    assert(tud_hid_get_report_cb(0u, 0u, HID_REPORT_TYPE_INPUT, (uint8_t *)&response, sizeof(response)) == sizeof(response));
    assert(response.opcode == USB_HID_OPCODE_SPI_MONITOR_GET_ALL_STATUS);
    assert(response.status == USB_HID_STATUS_BUSY);
    assert(response.payload_length == 0u);
}

int main(void) {
    test_spi_monitor_bus_config_updates_all_bus_channels();
    test_spi_monitor_allows_different_buses_to_use_different_modes();
    test_spi_monitor_stopping_bus_clears_all_bus_channels();
    test_spi_monitor_bus_config_can_select_one_channel();
    test_spi_monitor_bus_config_rejects_empty_channel_selection();
    test_spi_monitor_emits_mosi_miso_trace_packet();
    test_spi_monitor_emits_mosi_only_trace_packet();
    test_spi_monitor_stream_disable_blocks_ring_output();
    test_spi_monitor_set_bus_config_reports_disabled_when_stream_off();
    test_spi_monitor_disable_ignores_mode_and_channel_validation();
    test_spi_monitor_reconfig_failure_leaves_bus_stopped();
    test_spi_monitor_invalid_reconfig_does_not_flush_transaction();
    test_spi_monitor_open_transaction_does_not_count_packet_before_ring_push();
    test_spi_monitor_idle_cs_handoff_drops_inactive_words();
    test_spi_monitor_poll_flushes_short_dma_progress();
    test_spi_monitor_poll_flushes_independent_same_bus_channel_dma_progress();
    test_spi_monitor_directional_overruns_are_aggregated_in_status();
    test_spi_monitor_poll_timeout_refreshes_channel_overrun_status();
    test_spi_monitor_poll_timeout_ignores_stale_now_snapshot();
    test_spi_monitor_boundary_offset_splits_same_channel_transactions();
    test_spi_monitor_overflow_preserves_continued_for_same_transaction();
    test_cli_unknown_command_writes_fixed_helper();
    test_cli_help_writes_response_without_connected_flag();
    test_cli_help_is_flushed_after_temporary_tx_backpressure();
    test_cdc_rx_callback_leaves_remaining_packets_for_later_pass();
    test_cdc_rx_callback_preserves_packet_tail_when_queue_fills();
    test_cdc_tx_poll_limits_one_pass_budget();
    test_cdc_tx_enqueue_failure_is_counted();
    test_cli_version_reports_firmware_version();
    test_cli_led_command_and_hid_led_command_share_action();
    test_cli_reboot_command_uses_system_reboot();
    test_cli_i2cmon_command_updates_monitor_channel();
    test_cli_i2cmon_status_reports_channel_state();
    test_cli_i2cmon_reports_disabled_state();
    test_cli_spimon_command_updates_monitor_bus();
    test_cli_spimon_command_updates_one_selected_channel();
    test_cli_spimon_status_reports_bus_state();
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
    test_usb_bulk_poll_stream_clamps_invalid_payload_len();
    test_usb_bulk_poll_stream_writes_unaligned_whole_packet_tail();
    test_usb_bulk_poll_stream_emits_nothing_when_ring_empty();
    test_usb_bulk_service_stream_counts_host_backpressure_stalls();
    test_usb_bulk_service_stream_uses_all_available_vendor_space();
    test_usb_bulk_service_stream_uses_partial_vendor_space();
    test_usb_bulk_flush_flushes_vendor_endpoint();
    test_hid_builtin_command_returns_status_response();
    test_hid_stream_command_updates_shared_state();
    test_hid_i2c_monitor_set_rate_updates_channel();
    test_hid_i2c_monitor_set_rate_reports_busy();
    test_hid_i2c_monitor_get_status_returns_payload();
    test_hid_i2c_monitor_get_all_status_returns_all_channels();
    test_hid_i2c_monitor_get_all_status_rejects_failed_snapshot();
    test_hid_i2c_monitor_get_status_reports_invalid_channel();
    test_hid_i2c_monitor_get_all_status_reports_busy();
    test_hid_spi_monitor_set_config_updates_bus();
    test_hid_spi_monitor_set_config_reports_busy();
    test_hid_spi_monitor_get_status_returns_bus_payload();
    test_hid_spi_monitor_get_all_status_returns_all_channels();
    test_hid_spi_monitor_get_all_status_rejects_failed_snapshot();
    test_hid_spi_monitor_get_status_reports_invalid_bus();
    test_hid_spi_monitor_get_all_status_reports_busy();
    test_hid_reboot_command_runs_once();
    test_hid_second_report_is_rejected_without_overwriting_pending_command();
    test_hid_unknown_opcode_is_counted();
    return 0;
}