#include "usb/usb_hid.h"

#include "tusb.h"

#include <string.h>

#include "app_control.h"

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