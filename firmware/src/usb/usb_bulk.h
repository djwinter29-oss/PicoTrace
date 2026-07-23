/**
 * @file usb_bulk.h
 * @brief TinyUSB vendor bulk helpers for trace packet streaming.
 */

#ifndef USB_BULK_H
#define USB_BULK_H

#include <stdbool.h>
#include <stdint.h>

/** @brief Maximum vendor-stream bytes queued before control paths are serviced again. */
#define USB_BULK_SERVICE_QUANTUM_BYTES 1024u

/** @brief Maximum vendor-stream quanta serviced in one main-loop iteration. */
#define USB_BULK_SERVICE_MAX_PASSES 16u

/**
 * @brief Service the vendor bulk trace stream without forcing a flush.
 * @param enabled Shared stream enable state.
 * @return `true` when one or more bytes were queued to TinyUSB during this poll.
 */
bool usb_bulk_service_stream(bool enabled);

/** @brief Flush any pending vendor bulk bytes through TinyUSB. */
void usb_bulk_flush(void);

/** @brief Return the number of trace-stream stalls caused by no vendor write space since boot. */
uint32_t usb_bulk_host_backpressure_stall_count(void);

#endif