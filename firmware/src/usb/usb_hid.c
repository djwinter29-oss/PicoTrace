#include "usb/usb_hid.h"

#include "tusb.h"

#include <string.h>

#include "app_control.h"
#include "config/i2c_monitor_config.h"
#include "trace/capture/i2c_monitor_control.h"

#define USB_HID_I2C_MONITOR_SET_PAYLOAD_BYTES 5u
#define USB_HID_I2C_MONITOR_STATUS_PAYLOAD_BYTES 16u
#define USB_HID_I2C_MONITOR_ALL_STATUS_CHANNEL_BYTES 12u
#define USB_HID_I2C_MONITOR_ALL_STATUS_PAYLOAD_BYTES (I2C_MONITOR_CHANNEL_COUNT * USB_HID_I2C_MONITOR_ALL_STATUS_CHANNEL_BYTES)

static uint32_t usb_hid_read_u32_le(const uint8_t *data) {
    return ((uint32_t)data[0]) |
        (((uint32_t)data[1]) << 8u) |
        (((uint32_t)data[2]) << 16u) |
        (((uint32_t)data[3]) << 24u);
}

static void usb_hid_write_u32_le(uint8_t *data, uint32_t value) {
    data[0] = (uint8_t)(value & 0xFFu);
    data[1] = (uint8_t)((value >> 8u) & 0xFFu);
    data[2] = (uint8_t)((value >> 16u) & 0xFFu);
    data[3] = (uint8_t)((value >> 24u) & 0xFFu);
}

static bool usb_hid_handle_i2c_monitor_set_rate(usb_hid_command_t *response, const usb_hid_command_t *request) {
    uint32_t sample_hz;

    if (request->payload_length < USB_HID_I2C_MONITOR_SET_PAYLOAD_BYTES) {
        response->status = USB_HID_STATUS_BAD_LENGTH;
        return false;
    }

    sample_hz = usb_hid_read_u32_le(&request->payload[1]);
    if (!i2c_monitor_control_set_channel_sample_hz(request->payload[0], sample_hz)) {
        response->status = USB_HID_STATUS_REJECTED;
        return false;
    }

    return true;
}

static bool usb_hid_handle_i2c_monitor_get_status(usb_hid_command_t *response, const usb_hid_command_t *request) {
    i2c_monitor_channel_status_t status;

    if (request->payload_length < 1u) {
        response->status = USB_HID_STATUS_BAD_LENGTH;
        return false;
    }

    if (!i2c_monitor_control_get_channel_status(request->payload[0], &status)) {
        response->status = USB_HID_STATUS_REJECTED;
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
    return true;
}

static bool usb_hid_handle_i2c_monitor_get_all_status(usb_hid_command_t *response) {
    uint32_t channel;

    response->payload_length = USB_HID_I2C_MONITOR_ALL_STATUS_PAYLOAD_BYTES;
    for (channel = 0u; channel < I2C_MONITOR_CHANNEL_COUNT; ++channel) {
        i2c_monitor_channel_status_t status;
        uint8_t *payload = &response->payload[channel * USB_HID_I2C_MONITOR_ALL_STATUS_CHANNEL_BYTES];

        if (!i2c_monitor_control_get_channel_status(channel, &status)) {
            response->status = USB_HID_STATUS_REJECTED;
            response->payload_length = 0u;
            return false;
        }

        payload[0] = (uint8_t)channel;
        payload[1] = status.initialized ? 1u : 0u;
        payload[2] = status.running ? 1u : 0u;
        payload[3] = status.overrun ? 1u : 0u;
        usb_hid_write_u32_le(&payload[4], status.sample_hz);
        usb_hid_write_u32_le(&payload[8], status.overrun_count);
    }

    return true;
}

static usb_hid_command_t hid_command_state;
static usb_hid_command_t hid_report_state;
static bool hid_command_pending;
static bool hid_response_ready;

bool usb_hid_take_command(usb_hid_command_t *command) {
    if ((command == NULL) || !hid_command_pending) {
        return false;
    }

    *command = hid_command_state;
    hid_command_pending = false;
    return true;
}

void usb_hid_set_response(const usb_hid_command_t *response) {
    if (response == NULL) {
        return;
    }

    hid_report_state = *response;
    hid_response_ready = true;
}

void usb_hid_prepare_response(usb_hid_command_t *response, const usb_hid_command_t *request, usb_hid_status_t status) {
    memset(response, 0, sizeof(*response));
    response->opcode = request->opcode;
    response->sequence = request->sequence;
    response->status = (uint8_t)status;
}

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
        response.payload_length = 1u;
        response.payload[0] = app_control_stream_enabled() ? 1u : 0u;
        break;
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