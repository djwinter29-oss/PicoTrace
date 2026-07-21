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
 * @brief Service the vendor bulk trace stream, including any needed flush.
 * @param enabled Shared stream enable state.
 * @return `true` when one or more bytes were queued to TinyUSB during this poll.
 */
bool usb_bulk_service_stream(bool enabled);

/** @brief Flush any pending vendor bulk bytes through TinyUSB. */
void usb_bulk_flush(void);

/** @brief Return the number of trace-stream stalls caused by no vendor write space since boot. */
uint32_t usb_bulk_host_backpressure_stall_count(void);

/** @brief Return the number of trace-stream writes that deferred bytes due to stream write policy since boot. */
uint32_t usb_bulk_policy_deferral_count(void);

/** @brief Return the number of stream bytes deferred by vendor packet-alignment policy since boot. */
uint32_t usb_bulk_deferred_bytes_count(void);

#endif