/**
 * @file device_control.c
 * @brief Transport-neutral helpers for shared device, I2C, and SPI control actions.
 */

#include "device_control.h"

#include <stdio.h>
#include <stddef.h>

#include "app_control.h"

const char device_control_cli_help_command_help[] = "help";
const char device_control_cli_i2cmon_command_help[] = "i2cmon <channel> <sample_hz>|status <channel>";
const char device_control_cli_led_command_help[] = "led on|off";
const char device_control_cli_stream_command_help[] = "stream on|off";
const char device_control_cli_reboot_command_help[] = "reboot";
const char device_control_cli_spimon_command_help[] = "spimon <bus> off|mosi|both|status";
const char device_control_cli_version_command_help[] = "version";
const char device_control_cli_commands_header[] = "Commands:";
const char device_control_cli_spimon_usage_line[] = "Usage: spimon <bus> off|mosi <all|channel> <mode> [timeout_us]|both <all|channel> <mode> [timeout_us]|status <bus>";
const char device_control_cli_unknown_line[] = "Unknown command.";
const char device_control_cli_unknown_message[] = "Unknown command. Type help.";

static void device_control_write_u32_le(uint8_t *data, uint32_t value) {
    data[0] = (uint8_t)(value & 0xFFu);
    data[1] = (uint8_t)((value >> 8u) & 0xFFu);
    data[2] = (uint8_t)((value >> 16u) & 0xFFu);
    data[3] = (uint8_t)((value >> 24u) & 0xFFu);
}

static device_control_result_t device_control_result_from_i2c_rc(i2c_monitor_rc_t result) {
    switch (result) {
        case I2C_MONITOR_RC_OK:
            return DEVICE_CONTROL_RESULT_OK;
        case I2C_MONITOR_RC_BUSY:
            return DEVICE_CONTROL_RESULT_BUSY;
        case I2C_MONITOR_RC_DISABLED:
            return DEVICE_CONTROL_RESULT_DISABLED;
        case I2C_MONITOR_RC_INVALID:
            return DEVICE_CONTROL_RESULT_INVALID;
        case I2C_MONITOR_RC_FAILED:
        default:
            return DEVICE_CONTROL_RESULT_REJECTED;
    }
}

static device_control_result_t device_control_result_from_spi_rc(spi_monitor_rc_t result) {
    switch (result) {
        case SPI_MONITOR_RC_OK:
            return DEVICE_CONTROL_RESULT_OK;
        case SPI_MONITOR_RC_BUSY:
            return DEVICE_CONTROL_RESULT_BUSY;
        case SPI_MONITOR_RC_DISABLED:
            return DEVICE_CONTROL_RESULT_DISABLED;
        case SPI_MONITOR_RC_INVALID:
            return DEVICE_CONTROL_RESULT_INVALID;
        case SPI_MONITOR_RC_FAILED:
        default:
            return DEVICE_CONTROL_RESULT_REJECTED;
    }
}

static const char *device_control_spi_capture_name(spi_monitor_capture_t capture) {
    switch (capture) {
        case SPI_MONITOR_CAPTURE_MOSI:
            return "mosi";
        case SPI_MONITOR_CAPTURE_MOSI_MISO:
            return "both";
        case SPI_MONITOR_CAPTURE_DISABLED:
        default:
            return "off";
    }
}

static const char *device_control_spi_channel_select_name(uint8_t channel_select_mask) {
    switch (channel_select_mask) {
        case 0x01u:
            return "ch0";
        case 0x02u:
            return "ch1";
        case 0x04u:
            return "ch2";
        case SPI_MONITOR_CHANNEL_SELECT_ALL:
            return "all";
        default:
            return "mixed";
    }
}

device_control_status_t device_control_get_status(void) {
    device_control_status_t status = {
        .stream_enabled = app_control_stream_enabled(),
        .firmware_version = app_control_firmware_version(),
    };

    return status;
}

uint8_t device_control_get_device_status_payload_length(void) {
    device_control_status_t status = device_control_get_status();
    size_t version_length = 0u;

    while ((status.firmware_version[version_length] != '\0') && (version_length < DEVICE_CONTROL_DEVICE_STATUS_MAX_VERSION_BYTES)) {
        version_length += 1u;
    }

    return (uint8_t)(DEVICE_CONTROL_DEVICE_STATUS_FIXED_BYTES + version_length);
}

uint8_t device_control_encode_device_status_payload(uint8_t *payload, uint32_t capacity) {
    device_control_status_t status = device_control_get_status();
    uint8_t payload_length;
    uint32_t version_length;

    if (payload == NULL) {
        return 0u;
    }

    payload_length = device_control_get_device_status_payload_length();
    if (capacity < payload_length) {
        return 0u;
    }

    version_length = (uint32_t)(payload_length - DEVICE_CONTROL_DEVICE_STATUS_FIXED_BYTES);
    payload[0] = status.stream_enabled ? 1u : 0u;
    payload[1] = (uint8_t)version_length;
    for (uint32_t index = 0u; index < version_length; ++index) {
        payload[2u + index] = (uint8_t)status.firmware_version[index];
    }

    return payload_length;
}

uint8_t device_control_result_to_hid_status(device_control_result_t result) {
    switch (result) {
        case DEVICE_CONTROL_RESULT_OK:
            return 0u;
        case DEVICE_CONTROL_RESULT_BUSY:
            return 5u;
        case DEVICE_CONTROL_RESULT_INVALID:
            return 2u;
        case DEVICE_CONTROL_RESULT_DISABLED:
        case DEVICE_CONTROL_RESULT_REJECTED:
        default:
            return 4u;
    }
}

const char *device_control_i2c_apply_error_line(device_control_result_t result) {
    switch (result) {
        case DEVICE_CONTROL_RESULT_BUSY:
            return "i2cmon busy";
        case DEVICE_CONTROL_RESULT_DISABLED:
            return "i2cmon disabled";
        default:
            return "i2cmon apply failed";
    }
}

const char *device_control_spi_apply_error_line(device_control_result_t result) {
    switch (result) {
        case DEVICE_CONTROL_RESULT_BUSY:
            return "spimon busy";
        case DEVICE_CONTROL_RESULT_DISABLED:
            return "spimon disabled";
        default:
            return "spimon apply failed";
    }
}

bool device_control_format_version_line(char *buffer, size_t capacity) {
    if ((buffer == NULL) || (capacity == 0u)) {
        return false;
    }

    return snprintf(buffer, capacity, "firmware_version=%s", device_control_get_status().firmware_version) > 0;
}

const char *device_control_led_on_line(void) {
    return "LED on";
}

const char *device_control_led_off_line(void) {
    return "LED off";
}

const char *device_control_stream_on_line(void) {
    return "Stream on";
}

const char *device_control_stream_off_line(void) {
    return "Stream off";
}

void device_control_set_stream_enabled(bool enabled) {
    app_control_set_stream_enabled(enabled);
}

void device_control_set_led(bool on) {
    app_control_set_led(on);
}

void device_control_reboot(void) {
    app_control_reboot();
}

i2c_monitor_rc_t device_control_i2c_set_channel_sample_hz(uint32_t channel, uint32_t sample_hz) {
    return i2c_monitor_control_set_channel_sample_hz(channel, sample_hz);
}

device_control_result_t device_control_i2c_configure_channel(uint32_t channel, uint32_t sample_hz, i2c_monitor_channel_status_t *status_out) {
    i2c_monitor_rc_t result = i2c_monitor_control_set_channel_sample_hz(channel, sample_hz);

    if (result != I2C_MONITOR_RC_OK) {
        return device_control_result_from_i2c_rc(result);
    }

    if (status_out == NULL) {
        return DEVICE_CONTROL_RESULT_OK;
    }

    return device_control_i2c_read_channel_status(channel, status_out);
}

i2c_monitor_rc_t device_control_i2c_get_channel_status(uint32_t channel, i2c_monitor_channel_status_t *status_out) {
    return i2c_monitor_control_get_channel_status(channel, status_out);
}

device_control_result_t device_control_i2c_read_channel_status(uint32_t channel, i2c_monitor_channel_status_t *status_out) {
    return device_control_result_from_i2c_rc(i2c_monitor_control_get_channel_status(channel, status_out));
}

i2c_monitor_rc_t device_control_i2c_get_all_status(i2c_monitor_channel_status_t *status_out) {
    return i2c_monitor_control_get_all_status(status_out);
}

device_control_result_t device_control_i2c_read_all_status(i2c_monitor_channel_status_t *status_out) {
    return device_control_result_from_i2c_rc(i2c_monitor_control_get_all_status(status_out));
}

uint8_t device_control_encode_i2c_channel_status_payload(uint32_t channel, const i2c_monitor_channel_status_t *status, uint8_t *payload, uint32_t capacity) {
    if ((status == NULL) || (payload == NULL) || (capacity < DEVICE_CONTROL_I2C_MONITOR_STATUS_PAYLOAD_BYTES)) {
        return 0u;
    }

    payload[0] = (uint8_t)channel;
    payload[1] = status->initialized ? 1u : 0u;
    payload[2] = status->running ? 1u : 0u;
    payload[3] = status->overrun ? 1u : 0u;
    device_control_write_u32_le(&payload[4], status->sample_hz);
    device_control_write_u32_le(&payload[8], status->completed_buffers);
    device_control_write_u32_le(&payload[12], status->overrun_count);
    payload[16] = status->transition_pending ? 1u : 0u;
    payload[17] = status->transition_reason;
    return DEVICE_CONTROL_I2C_MONITOR_STATUS_PAYLOAD_BYTES;
}

bool device_control_format_i2c_channel_status_line(uint32_t channel, const i2c_monitor_channel_status_t *status, char *buffer, size_t capacity) {
    if ((status == NULL) || (buffer == NULL) || (capacity == 0u)) {
        return false;
    }

    return snprintf(
        buffer,
        capacity,
        "i2cmon ch%lu %s hz=%lu buffers=%lu overruns=%lu sticky=%u pending=%u reason=%u",
        (unsigned long)channel,
        status->running ? "running" : "stopped",
        (unsigned long)status->sample_hz,
        (unsigned long)status->completed_buffers,
        (unsigned long)status->overrun_count,
        status->overrun ? 1u : 0u,
        status->transition_pending ? 1u : 0u,
        (unsigned int)status->transition_reason
    ) > 0;
}

uint8_t device_control_encode_i2c_all_status_payload(const i2c_monitor_channel_status_t *status, uint32_t status_count, uint8_t *payload, uint32_t capacity) {
    if ((status == NULL) || (payload == NULL) || (status_count < I2C_MONITOR_CHANNEL_COUNT) || (capacity < DEVICE_CONTROL_I2C_MONITOR_ALL_STATUS_PAYLOAD_BYTES)) {
        return 0u;
    }

    for (uint32_t channel = 0u; channel < I2C_MONITOR_CHANNEL_COUNT; ++channel) {
        uint8_t *channel_payload = &payload[channel * DEVICE_CONTROL_I2C_MONITOR_ALL_STATUS_CHANNEL_BYTES];

        channel_payload[0] = (uint8_t)channel;
        channel_payload[1] = status[channel].initialized ? 1u : 0u;
        channel_payload[2] = status[channel].running ? 1u : 0u;
        channel_payload[3] = status[channel].overrun ? 1u : 0u;
        device_control_write_u32_le(&channel_payload[4], status[channel].sample_hz);
        device_control_write_u32_le(&channel_payload[8], status[channel].overrun_count);
        channel_payload[12] = status[channel].transition_pending ? 1u : 0u;
        channel_payload[13] = status[channel].transition_reason;
    }

    return DEVICE_CONTROL_I2C_MONITOR_ALL_STATUS_PAYLOAD_BYTES;
}

spi_monitor_rc_t device_control_spi_set_bus_config(uint32_t bus, const spi_monitor_bus_config_t *config) {
    return spi_monitor_control_set_bus_config(bus, config);
}

device_control_result_t device_control_spi_configure_bus(uint32_t bus, const spi_monitor_bus_config_t *config, spi_monitor_bus_status_t *status_out) {
    spi_monitor_rc_t result = spi_monitor_control_set_bus_config(bus, config);

    if (result != SPI_MONITOR_RC_OK) {
        return device_control_result_from_spi_rc(result);
    }

    if (status_out == NULL) {
        return DEVICE_CONTROL_RESULT_OK;
    }

    return device_control_spi_read_bus_status(bus, status_out);
}

spi_monitor_rc_t device_control_spi_get_bus_status(uint32_t bus, spi_monitor_bus_status_t *status_out) {
    return spi_monitor_control_get_bus_status(bus, status_out);
}

device_control_result_t device_control_spi_read_bus_status(uint32_t bus, spi_monitor_bus_status_t *status_out) {
    return device_control_result_from_spi_rc(spi_monitor_control_get_bus_status(bus, status_out));
}

spi_monitor_rc_t device_control_spi_get_all_status(spi_monitor_channel_status_t *status_out) {
    return spi_monitor_control_get_all_status(status_out);
}

device_control_result_t device_control_spi_read_all_status(spi_monitor_channel_status_t *status_out) {
    return device_control_result_from_spi_rc(spi_monitor_control_get_all_status(status_out));
}

uint8_t device_control_encode_spi_bus_status_payload(uint32_t bus, const spi_monitor_bus_status_t *status, uint8_t *payload, uint32_t capacity) {
    if ((status == NULL) || (payload == NULL) || (capacity < DEVICE_CONTROL_SPI_MONITOR_STATUS_PAYLOAD_BYTES)) {
        return 0u;
    }

    payload[0] = (uint8_t)bus;
    payload[1] = status->initialized ? 1u : 0u;
    payload[2] = status->running ? 1u : 0u;
    payload[3] = (uint8_t)status->capture;
    payload[4] = status->spi_mode;
    payload[5] = status->channel_select_mask;
    device_control_write_u32_le(&payload[6], status->timeout_us);
    device_control_write_u32_le(&payload[10], status->packets_emitted);
    device_control_write_u32_le(&payload[14], status->transactions_emitted);
    device_control_write_u32_le(&payload[18], status->overrun_count);
    device_control_write_u32_le(&payload[22], status->sink_overrun_count);
    device_control_write_u32_le(&payload[26], status->sampler_overrun_count);
    device_control_write_u32_le(&payload[30], status->ring_drop_count);
    device_control_write_u32_le(&payload[34], status->usb_host_backpressure_stall_count);
    device_control_write_u32_le(&payload[38], status->peak_ring_depth_packets);
    device_control_write_u32_le(&payload[42], status->timeout_close_count);
    return DEVICE_CONTROL_SPI_MONITOR_STATUS_PAYLOAD_BYTES;
}

bool device_control_format_spi_bus_status_line(uint32_t bus, const spi_monitor_bus_status_t *status, char *buffer, size_t capacity) {
    if ((status == NULL) || (buffer == NULL) || (capacity == 0u)) {
        return false;
    }

    return snprintf(
        buffer,
        capacity,
        "spimon bus%lu %s select=%s capture=%s mode=%u timeout_us=%lu packets=%lu txns=%lu overruns=%lu timeout_closes=%lu",
        (unsigned long)bus,
        status->running ? "running" : "stopped",
        device_control_spi_channel_select_name(status->channel_select_mask),
        device_control_spi_capture_name(status->capture),
        (unsigned int)status->spi_mode,
        (unsigned long)status->timeout_us,
        (unsigned long)status->packets_emitted,
        (unsigned long)status->transactions_emitted,
        (unsigned long)status->overrun_count,
        (unsigned long)status->timeout_close_count
    ) > 0;
}

uint8_t device_control_encode_spi_all_status_payload(const spi_monitor_channel_status_t *status, uint32_t status_count, uint8_t *payload, uint32_t capacity) {
    if ((status == NULL) || (payload == NULL) || (status_count < SPI_MONITOR_CHANNEL_COUNT) || (capacity < DEVICE_CONTROL_SPI_MONITOR_ALL_STATUS_PAYLOAD_BYTES)) {
        return 0u;
    }

    for (uint32_t channel = 0u; channel < SPI_MONITOR_CHANNEL_COUNT; ++channel) {
        uint8_t *channel_payload = &payload[channel * DEVICE_CONTROL_SPI_MONITOR_ALL_STATUS_CHANNEL_BYTES];

        channel_payload[0] = (uint8_t)channel;
        channel_payload[1] = status[channel].initialized ? 1u : 0u;
        channel_payload[2] = status[channel].running ? 1u : 0u;
        channel_payload[3] = (uint8_t)status[channel].capture;
        channel_payload[4] = status[channel].spi_mode;
        device_control_write_u32_le(&channel_payload[5], status[channel].timeout_us);
        channel_payload[9] = (status[channel].overrun_count != 0u) ? 1u : 0u;
    }

    return DEVICE_CONTROL_SPI_MONITOR_ALL_STATUS_PAYLOAD_BYTES;
}