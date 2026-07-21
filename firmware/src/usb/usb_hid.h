#ifndef USB_HID_H
#define USB_HID_H

#include <stdbool.h>
#include <stdint.h>

/** @brief Fixed HID report size used by the PicoTrace control channel. */
#define USB_HID_REPORT_SIZE 64u

/** @brief HID opcodes understood by the shared PicoTrace control endpoint. */
typedef enum {
	USB_HID_OPCODE_NOP = 0u, /**< No-op command used for transport checks. */
	USB_HID_OPCODE_GET_STATUS = 1u, /**< Read shared device status such as stream enable state. */
	USB_HID_OPCODE_STREAM_ENABLE = 2u, /**< Enable vendor bulk streaming. */
	USB_HID_OPCODE_STREAM_DISABLE = 3u, /**< Disable vendor bulk streaming. */
	USB_HID_OPCODE_I2C_MONITOR_SET_RATE = 4u, /**< Start, stop, or retune one I2C monitor channel. */
	USB_HID_OPCODE_I2C_MONITOR_GET_STATUS = 5u, /**< Read one I2C monitor channel status snapshot. */
	USB_HID_OPCODE_I2C_MONITOR_GET_ALL_STATUS = 6u, /**< Read all I2C monitor channel status snapshots. */
	USB_HID_OPCODE_SPI_MONITOR_SET_CONFIG = 7u, /**< Start, stop, or reconfigure one observed SPI bus. */
	USB_HID_OPCODE_SPI_MONITOR_GET_STATUS = 8u, /**< Read one observed SPI bus status snapshot. */
	USB_HID_OPCODE_SPI_MONITOR_GET_ALL_STATUS = 9u, /**< Read all SPI monitor channel status snapshots. */
	USB_HID_OPCODE_LED_ON = 0x80u, /**< Turn the board status LED on. */
	USB_HID_OPCODE_LED_OFF = 0x81u, /**< Turn the board status LED off. */
	USB_HID_OPCODE_REBOOT = 0x82u, /**< Reboot the board through the control path. */
	USB_HID_OPCODE_USER_BASE = 0x80u, /**< First opcode reserved for user or board-local extensions. */
} usb_hid_opcode_t;

/** @brief Status codes returned in HID command responses. */
typedef enum {
	USB_HID_STATUS_OK = 0u, /**< Command completed successfully. */
	USB_HID_STATUS_UNKNOWN_COMMAND = 1u, /**< Opcode is not recognized by the device. */
	USB_HID_STATUS_BAD_LENGTH = 2u, /**< Payload length is too short for the opcode. */
	USB_HID_STATUS_PENDING = 3u, /**< Reserved status for deferred completion paths. */
	USB_HID_STATUS_REJECTED = 4u, /**< Command was valid but rejected by policy or monitor state. */
	USB_HID_STATUS_BUSY = 5u, /**< Command could not run because the target monitor is busy. */
} usb_hid_status_t;

/** @brief Fixed-format HID command or response packet. */
typedef struct {
	uint8_t opcode; /**< Operation code identifying the command family. */
	uint8_t sequence; /**< Caller-owned sequence number echoed in the response. */
	uint8_t status; /**< Response status code, or `0` in outbound command requests. */
	uint8_t payload_length; /**< Number of valid bytes in @ref payload. */
	uint8_t payload[USB_HID_REPORT_SIZE - 4u]; /**< Opcode-specific request or response bytes. */
} usb_hid_command_t;

/** @brief Lightweight HID observability counters for control-path debugging. */
typedef struct {
	uint32_t busy_rejects; /**< Incoming reports rejected because the single command slot was occupied. */
	uint32_t unknown_opcodes; /**< Incoming reports rejected because their opcode was not recognized. */
} usb_hid_stats_t;

_Static_assert(sizeof(usb_hid_command_t) == USB_HID_REPORT_SIZE, "usb_hid_command_t must match the HID report size");

/**
 * @brief Service one pending HID command on the USB-owning core.
 */
void usb_hid_poll(void);

/**
 * @brief Return the current HID observability counters.
 */
usb_hid_stats_t usb_hid_get_stats(void);

/**
 * @brief Clear the HID observability counters.
 */
void usb_hid_reset_stats(void);

#endif