/**
 * @file usb_bulk.h
 * @brief TinyUSB vendor bulk helpers for trace packet streaming.
 */

#ifndef USB_BULK_H
#define USB_BULK_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Attempt to write one exact vendor bulk payload immediately.
 * @param data Caller-owned payload bytes.
 * @param length Number of bytes to write.
 * @return `true` when all bytes were accepted, otherwise `false`.
 */
bool usb_bulk_write(const uint8_t *data, uint32_t length);

/**
 * @brief Attempt a stream-oriented vendor bulk write.
 * @param data Caller-owned payload bytes.
 * @param length Number of bytes available for streaming.
 * @return Number of bytes actually written.
 */
uint32_t usb_bulk_stream_write(const uint8_t *data, uint32_t length);

/**
 * @brief Drain queued trace packets onto the vendor bulk endpoint when streaming is enabled.
 * @param enabled Shared stream enable state.
 * @return `true` when one or more bytes were queued to TinyUSB during this poll.
 */
bool usb_bulk_poll_stream(bool enabled);

/** @brief Flush any pending vendor bulk bytes through TinyUSB. */
void usb_bulk_flush(void);

/** @brief Return the number of stalled trace-stream write attempts since boot. */
uint32_t usb_bulk_stall_count(void);

#endif