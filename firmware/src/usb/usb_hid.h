#ifndef USB_HID_H
#define USB_HID_H

#include <stdbool.h>
#include <stdint.h>

#define USB_HID_REPORT_SIZE 64u

typedef enum {
	USB_HID_OPCODE_NOP = 0u,
	USB_HID_OPCODE_GET_STATUS = 1u,
	USB_HID_OPCODE_STREAM_ENABLE = 2u,
	USB_HID_OPCODE_STREAM_DISABLE = 3u,
	USB_HID_OPCODE_LED_ON = 0x80u,
	USB_HID_OPCODE_LED_OFF = 0x81u,
	USB_HID_OPCODE_REBOOT = 0x82u,
	USB_HID_OPCODE_USER_BASE = 0x80u,
} usb_hid_opcode_t;

typedef enum {
	USB_HID_STATUS_OK = 0u,
	USB_HID_STATUS_UNKNOWN_COMMAND = 1u,
	USB_HID_STATUS_BAD_LENGTH = 2u,
	USB_HID_STATUS_PENDING = 3u,
} usb_hid_status_t;

typedef struct {
	uint8_t opcode;
	uint8_t sequence;
	uint8_t status;
	uint8_t payload_length;
	uint8_t payload[USB_HID_REPORT_SIZE - 4u];
} usb_hid_command_t;

_Static_assert(sizeof(usb_hid_command_t) == USB_HID_REPORT_SIZE, "usb_hid_command_t must match the HID report size");

bool usb_hid_take_command(usb_hid_command_t *command);
void usb_hid_set_response(const usb_hid_command_t *response);
void usb_hid_prepare_response(usb_hid_command_t *response, const usb_hid_command_t *request, usb_hid_status_t status);
void usb_hid_poll(void);

#endif