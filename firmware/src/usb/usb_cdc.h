/**
 * @file usb_cdc.h
 * @brief TinyUSB CDC helpers used by the board-local CLI transport.
 */

#ifndef USB_CDC_H
#define USB_CDC_H

#include <stdbool.h>
#include <stdint.h>

/** @brief Lightweight CDC observability counters for control-path debugging. */
typedef struct {
	uint32_t rx_dropped_bytes; /**< Bytes discarded before reaching the local receive queue. */
	uint32_t tx_enqueue_failures; /**< Write requests that could not be queued for transmit. */
} usb_cdc_stats_t;

/** @brief Return whether the CDC interface is enumerated and the host is connected. */
bool usb_cdc_is_connected(void);

/**
 * @brief Read queued CDC bytes from the local receive queue.
 * @param data Caller-owned destination buffer.
 * @param capacity Maximum bytes to copy.
 * @return Number of bytes copied into @p data.
 */
uint32_t usb_cdc_read(uint8_t *data, uint32_t capacity);

/**
 * @brief Queue CDC transmit bytes and attempt to flush them immediately.
 * @param data Caller-owned payload bytes.
 * @param length Number of bytes to queue.
 * @return `true` when the payload was queued, otherwise `false`.
 */
bool usb_cdc_write(const uint8_t *data, uint32_t length);

/** @brief Service the queued CDC transmit buffer. */
void usb_cdc_poll_tx(void);

/** @brief Return the current CDC observability counters. */
usb_cdc_stats_t usb_cdc_get_stats(void);

/** @brief Clear the CDC observability counters. */
void usb_cdc_reset_stats(void);

#endif