#include "usb/usb_hid.h"

#include "tusb.h"

#include <stddef.h>
#include <string.h>

#include "app_control.h"
#include "config/i2c_monitor_config.h"
#include "config/spi_monitor_config.h"
#include "trace/capture/spi_monitor_control.h"
#include "trace/decode/i2c_decoder.h"
#include "trace/capture/i2c_monitor_control.h"

/** @brief Payload bytes required for the I2C set-rate request. */
#define USB_HID_I2C_MONITOR_SET_PAYLOAD_BYTES 5u
/** @brief Payload bytes returned for one I2C channel status snapshot. */
#define USB_HID_I2C_MONITOR_STATUS_PAYLOAD_BYTES 18u
/** @brief Bytes used per channel in the compact I2C all-status response. */
#define USB_HID_I2C_MONITOR_ALL_STATUS_CHANNEL_BYTES 14u
/** @brief Total payload bytes returned for the compact I2C all-status response. */
#define USB_HID_I2C_MONITOR_ALL_STATUS_PAYLOAD_BYTES (I2C_MONITOR_CHANNEL_COUNT * USB_HID_I2C_MONITOR_ALL_STATUS_CHANNEL_BYTES)
/** @brief Payload bytes required for the SPI set-config request. */
#define USB_HID_SPI_MONITOR_SET_PAYLOAD_BYTES 8u
/** @brief Payload bytes returned for one SPI bus status snapshot. */
#define USB_HID_SPI_MONITOR_STATUS_PAYLOAD_BYTES 18u
/** @brief Bytes used per channel in the compact SPI all-status response. */
#define USB_HID_SPI_MONITOR_ALL_STATUS_CHANNEL_BYTES 10u
/** @brief Total payload bytes returned for the compact SPI all-status response. */
#define USB_HID_SPI_MONITOR_ALL_STATUS_PAYLOAD_BYTES (SPI_MONITOR_CHANNEL_COUNT * USB_HID_SPI_MONITOR_ALL_STATUS_CHANNEL_BYTES)
/** @brief Fixed bytes used by the shared device status fields before the version string. */
#define USB_HID_DEVICE_STATUS_FIXED_BYTES 2u
/** @brief Maximum firmware version bytes returned in the shared device status payload. */
#define USB_HID_DEVICE_STATUS_MAX_VERSION_BYTES ((USB_HID_REPORT_SIZE - 4u) - USB_HID_DEVICE_STATUS_FIXED_BYTES)

/** @brief Decode a 32-bit little-endian value from a HID payload buffer. */
static uint32_t usb_hid_read_u32_le(const uint8_t *data) {
    return ((uint32_t)data[0]) |
        (((uint32_t)data[1]) << 8u) |
        (((uint32_t)data[2]) << 16u) |
        (((uint32_t)data[3]) << 24u);
}

    /** @brief Encode a 32-bit little-endian value into a HID payload buffer. */
static void usb_hid_write_u32_le(uint8_t *data, uint32_t value) {
    data[0] = (uint8_t)(value & 0xFFu);
    data[1] = (uint8_t)((value >> 8u) & 0xFFu);
    data[2] = (uint8_t)((value >> 16u) & 0xFFu);
    data[3] = (uint8_t)((value >> 24u) & 0xFFu);
}

/** @brief Apply one I2C monitor set-rate HID request. */
static bool usb_hid_handle_i2c_monitor_set_rate(usb_hid_command_t *response, const usb_hid_command_t *request) {
    i2c_monitor_rc_t result;
    uint32_t sample_hz;

    if (request->payload_length < USB_HID_I2C_MONITOR_SET_PAYLOAD_BYTES) {
        response->status = USB_HID_STATUS_BAD_LENGTH;
        return false;
    }

    sample_hz = usb_hid_read_u32_le(&request->payload[1]);
    result = i2c_monitor_control_set_channel_sample_hz(request->payload[0], sample_hz);
    if (result != I2C_MONITOR_RC_OK) {
        if (result == I2C_MONITOR_RC_BUSY) {
            response->status = USB_HID_STATUS_BUSY;
        } else {
            response->status = USB_HID_STATUS_REJECTED;
        }
        return false;
    }

    return true;
}

/** @brief Encode one I2C monitor status snapshot into a HID response payload. */
static bool usb_hid_handle_i2c_monitor_get_status(usb_hid_command_t *response, const usb_hid_command_t *request) {
    i2c_monitor_channel_status_t status;

    if (request->payload_length < 1u) {
        response->status = USB_HID_STATUS_BAD_LENGTH;
        return false;
    }

    if (i2c_monitor_control_get_channel_status(request->payload[0], &status) != I2C_MONITOR_RC_OK) {
        response->status = USB_HID_STATUS_REJECTED;
    /** @brief Encode all I2C monitor status snapshots into a compact HID response payload. */
        return false;
    }

    response->payload_length = USB_HID_I2C_MONITOR_STATUS_PAYLOAD_BYTES;
    response->payload[0] = request->payload[0];
    response->payload[1] = status.initialized ? 1u : 0u;
    response->payload[2] = status.running ? 1u : 0u;
    response->payload[3] = status.overrun ? 1u : 0u;
    usb_hid_write_u32_le(&response->payload[4], status.sample_hz);
    usb_hid_write_u32_le(&response->payload[8], status.completed_buffers);
    usb_hid_write_u32_le(&response->payload[12], status.overrun_count);
    response->payload[16] = status.transition_pending ? 1u : 0u;
    response->payload[17] = status.transition_reason;
    return true;
}

static bool usb_hid_handle_i2c_monitor_get_all_status(usb_hid_command_t *response) {
    i2c_monitor_channel_status_t status[I2C_MONITOR_CHANNEL_COUNT];
    uint32_t channel;

    if (i2c_monitor_control_get_all_status(status) != I2C_MONITOR_RC_OK) {
        response->status = USB_HID_STATUS_REJECTED;
        response->payload_length = 0u;
        return false;
    }

    response->payload_length = USB_HID_I2C_MONITOR_ALL_STATUS_PAYLOAD_BYTES;
    for (channel = 0u; channel < I2C_MONITOR_CHANNEL_COUNT; ++channel) {
        uint8_t *payload = &response->payload[channel * USB_HID_I2C_MONITOR_ALL_STATUS_CHANNEL_BYTES];

        payload[0] = (uint8_t)channel;
        payload[1] = status[channel].initialized ? 1u : 0u;
        payload[2] = status[channel].running ? 1u : 0u;
        payload[3] = status[channel].overrun ? 1u : 0u;
        usb_hid_write_u32_le(&payload[4], status[channel].sample_hz);
        usb_hid_write_u32_le(&payload[8], status[channel].overrun_count);
        payload[12] = status[channel].transition_pending ? 1u : 0u;
        payload[13] = status[channel].transition_reason;
    }

    return true;
}

/** @brief Apply one SPI monitor set-config HID request. */
static bool usb_hid_handle_spi_monitor_set_config(usb_hid_command_t *response, const usb_hid_command_t *request) {
    spi_monitor_bus_config_t config;
    spi_monitor_rc_t result;

    if (request->payload_length < USB_HID_SPI_MONITOR_SET_PAYLOAD_BYTES) {
        response->status = USB_HID_STATUS_BAD_LENGTH;
        return false;
    }

    config.capture = (spi_monitor_capture_t)request->payload[1];
    config.spi_mode = request->payload[2];
    config.channel_select_mask = request->payload[3];
    config.timeout_us = usb_hid_read_u32_le(&request->payload[4]);
    result = spi_monitor_control_set_bus_config(request->payload[0], &config);
    if (result != SPI_MONITOR_RC_OK) {
        if (result == SPI_MONITOR_RC_BUSY) {
            response->status = USB_HID_STATUS_BUSY;
        } else {
            response->status = USB_HID_STATUS_REJECTED;
        }
        return false;
    }

    return true;
}

/** @brief Encode one SPI monitor bus status snapshot into a HID response payload. */
static bool usb_hid_handle_spi_monitor_get_status(usb_hid_command_t *response, const usb_hid_command_t *request) {
    spi_monitor_bus_status_t status;

    if (request->payload_length < 1u) {
        response->status = USB_HID_STATUS_BAD_LENGTH;
        return false;
    }

    if (spi_monitor_control_get_bus_status(request->payload[0], &status) != SPI_MONITOR_RC_OK) {
        response->status = USB_HID_STATUS_REJECTED;
        return false;
    }

    response->payload_length = USB_HID_SPI_MONITOR_STATUS_PAYLOAD_BYTES;
    response->payload[0] = request->payload[0];
    response->payload[1] = status.initialized ? 1u : 0u;
    response->payload[2] = status.running ? 1u : 0u;
    response->payload[3] = (uint8_t)status.capture;
    response->payload[4] = status.spi_mode;
    response->payload[5] = status.channel_select_mask;
    usb_hid_write_u32_le(&response->payload[6], status.timeout_us);
    usb_hid_write_u32_le(&response->payload[10], status.packets_emitted);
    usb_hid_write_u32_le(&response->payload[14], status.overrun_count);
    return true;
}

static bool usb_hid_handle_spi_monitor_get_all_status(usb_hid_command_t *response) {
    spi_monitor_channel_status_t status[SPI_MONITOR_CHANNEL_COUNT];
    uint32_t channel;

    if (spi_monitor_control_get_all_status(status) != SPI_MONITOR_RC_OK) {
        response->status = USB_HID_STATUS_REJECTED;
        response->payload_length = 0u;
        return false;
    }

    response->payload_length = USB_HID_SPI_MONITOR_ALL_STATUS_PAYLOAD_BYTES;
    for (channel = 0u; channel < SPI_MONITOR_CHANNEL_COUNT; ++channel) {
        uint8_t *payload = &response->payload[channel * USB_HID_SPI_MONITOR_ALL_STATUS_CHANNEL_BYTES];

        payload[0] = (uint8_t)channel;
        payload[1] = status[channel].initialized ? 1u : 0u;
        payload[2] = status[channel].running ? 1u : 0u;
        payload[3] = (uint8_t)status[channel].capture;
        payload[4] = status[channel].spi_mode;
        usb_hid_write_u32_le(&payload[5], status[channel].timeout_us);
        payload[9] = (status[channel].overrun_count != 0u) ? 1u : 0u;
    }

    return true;
}

/** @brief Most recently received HID output report pending dispatch. */
static usb_hid_command_t hid_command_state;
/** @brief Most recently prepared HID input report returned to the host. */
static usb_hid_command_t hid_report_state;
/** @brief Indicates whether @ref hid_command_state currently holds an unread command. */
static bool hid_command_pending;
/** @brief Indicates whether @ref hid_report_state holds a prepared response. */
static bool hid_response_ready;

/** @copydoc usb_hid_take_command */
bool usb_hid_take_command(usb_hid_command_t *command) {
    if ((command == NULL) || !hid_command_pending) {
        return false;
    }

    *command = hid_command_state;
    hid_command_pending = false;
    return true;
}

/** @copydoc usb_hid_set_response */
void usb_hid_set_response(const usb_hid_command_t *response) {
    if (response == NULL) {
        return;
    }

    hid_report_state = *response;
    hid_response_ready = true;
}

/** @copydoc usb_hid_prepare_response */
void usb_hid_prepare_response(usb_hid_command_t *response, const usb_hid_command_t *request, usb_hid_status_t status) {
    memset(response, 0, sizeof(*response));
    response->opcode = request->opcode;
    response->sequence = request->sequence;
    response->status = (uint8_t)status;
}

/** @copydoc usb_hid_poll */
void usb_hid_poll(void) {
    usb_hid_command_t response;

    if (!hid_command_pending) {
        return;
    }

    hid_command_pending = false;

    usb_hid_prepare_response(&response, &hid_command_state, USB_HID_STATUS_OK);

    switch ((usb_hid_opcode_t)hid_command_state.opcode) {
    case USB_HID_OPCODE_NOP:
        break;
    case USB_HID_OPCODE_GET_STATUS:
    {
        const char *firmware_version = app_control_firmware_version();
        size_t version_length = strlen(firmware_version);

        if (version_length > USB_HID_DEVICE_STATUS_MAX_VERSION_BYTES) {
            version_length = USB_HID_DEVICE_STATUS_MAX_VERSION_BYTES;
        }

        response.payload_length = (uint8_t)(USB_HID_DEVICE_STATUS_FIXED_BYTES + version_length);
        response.payload[0] = app_control_stream_enabled() ? 1u : 0u;
        response.payload[1] = (uint8_t)version_length;
        memcpy(&response.payload[2], firmware_version, version_length);
        break;
    }
    case USB_HID_OPCODE_STREAM_ENABLE:
        app_control_set_stream_enabled(true);
        break;
    case USB_HID_OPCODE_STREAM_DISABLE:
        app_control_set_stream_enabled(false);
        break;
    case USB_HID_OPCODE_I2C_MONITOR_SET_RATE:
        (void)usb_hid_handle_i2c_monitor_set_rate(&response, &hid_command_state);
        break;
    case USB_HID_OPCODE_I2C_MONITOR_GET_STATUS:
        (void)usb_hid_handle_i2c_monitor_get_status(&response, &hid_command_state);
        break;
    case USB_HID_OPCODE_I2C_MONITOR_GET_ALL_STATUS:
        (void)usb_hid_handle_i2c_monitor_get_all_status(&response);
        break;
    case USB_HID_OPCODE_SPI_MONITOR_SET_CONFIG:
        (void)usb_hid_handle_spi_monitor_set_config(&response, &hid_command_state);
        break;
    case USB_HID_OPCODE_SPI_MONITOR_GET_STATUS:
        (void)usb_hid_handle_spi_monitor_get_status(&response, &hid_command_state);
        break;
    case USB_HID_OPCODE_SPI_MONITOR_GET_ALL_STATUS:
        (void)usb_hid_handle_spi_monitor_get_all_status(&response);
        break;
    case USB_HID_OPCODE_LED_ON:
        app_control_set_led(true);
        break;
    case USB_HID_OPCODE_LED_OFF:
        app_control_set_led(false);
        break;
    case USB_HID_OPCODE_REBOOT:
        app_control_reboot();
        break;
    default:
        response.status = USB_HID_STATUS_UNKNOWN_COMMAND;
        break;
    }

    usb_hid_set_response(&response);
}

uint16_t tud_hid_get_report_cb(
    uint8_t instance,
    uint8_t report_id,
    hid_report_type_t report_type,
    uint8_t *buffer,
    uint16_t reqlen
) {
    uint16_t copy_len = reqlen;

    (void)instance;
    (void)report_id;
    (void)report_type;

    if (copy_len > sizeof(hid_report_state)) {
        copy_len = (uint16_t)sizeof(hid_report_state);
    }

    (void)hid_response_ready;
    memcpy(buffer, &hid_report_state, copy_len);
    return copy_len;
}

void tud_hid_set_report_cb(
    uint8_t instance,
    uint8_t report_id,
    hid_report_type_t report_type,
    uint8_t const *buffer,
    uint16_t bufsize
) {
    uint16_t copy_len = bufsize;

    (void)instance;
    (void)report_id;
    (void)report_type;

    if (copy_len > sizeof(hid_report_state)) {
        copy_len = (uint16_t)sizeof(hid_report_state);
    }

    memset(&hid_command_state, 0, sizeof(hid_command_state));
    memcpy(&hid_command_state, buffer, copy_len);
    hid_command_pending = true;
}