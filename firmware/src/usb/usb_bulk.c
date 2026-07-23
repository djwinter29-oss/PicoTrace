/**
 * @file usb_bulk.c
 * @brief TinyUSB vendor bulk helpers for trace-ring streaming and bounded writes.
 */

#include "usb/usb_bulk.h"

#include "tusb.h"

#include <assert.h>
#include <stddef.h>

#include "trace/trace_packet.h"
#include "trace/trace_ring.h"

/** @brief TinyUSB vendor interface index used for trace streaming. */
#define USB_VENDOR_ITF 0u

/** @brief Stream-side packet and observability state retained across vendor drain polls. */
typedef struct {
    const trace_packet_t *packet; /**< Borrowed pointer to the trace packet currently being streamed. */
    uint32_t packet_offset; /**< Byte offset already transmitted from @ref packet. */
    uint32_t packet_bytes; /**< Cached total byte length for @ref packet. */
    uint32_t host_backpressure_stall_count; /**< Number of polls that found queued data but no vendor write space. */
} usb_bulk_stream_state_t;

/** @brief Persistent vendor stream state. */
static usb_bulk_stream_state_t g_usb_bulk_stream_state;

/** @brief Prepared view of the next consumer-side USB write span. */
typedef struct {
    const uint8_t *data; /**< Start of the current borrowed packet span to write. */
    uint32_t length; /**< Maximum bytes worth attempting to write for this span. */
} usb_bulk_stream_chunk_t;

/** @brief Drop any in-progress borrowed packet and restart stream state at a packet boundary. */
static void usb_bulk_reset_stream_state(void) {
    g_usb_bulk_stream_state.packet = NULL;
    g_usb_bulk_stream_state.packet_offset = 0u;
    g_usb_bulk_stream_state.packet_bytes = 0u;
}

/** @brief Return the next stream span that is ready to send, dropping invalid packets on the way. */
static bool usb_bulk_prepare_stream_chunk(uint32_t bytes_remaining, usb_bulk_stream_chunk_t *chunk_out) {
    while (g_usb_bulk_stream_state.packet == NULL) {
        const trace_packet_t *packet = trace_ring_peek();
        uint32_t payload_bytes;

        if (packet == NULL) {
            return false;
        }

        payload_bytes = packet->header.payload_len;
        assert(payload_bytes <= TRACE_PACKET_PAYLOAD_BYTES);
        if (payload_bytes > TRACE_PACKET_PAYLOAD_BYTES) {
            payload_bytes = TRACE_PACKET_PAYLOAD_BYTES;
        }

        g_usb_bulk_stream_state.packet = packet;
        g_usb_bulk_stream_state.packet_offset = 0u;
        g_usb_bulk_stream_state.packet_bytes = TRACE_PACKET_HEADER_BYTES + payload_bytes;
    }

    chunk_out->data = ((const uint8_t *)g_usb_bulk_stream_state.packet) + g_usb_bulk_stream_state.packet_offset;
    chunk_out->length = g_usb_bulk_stream_state.packet_bytes - g_usb_bulk_stream_state.packet_offset;
    if (chunk_out->length > bytes_remaining) {
        chunk_out->length = bytes_remaining;
    }
    return true;
}

/** @brief Attempt one vendor bulk stream write using all currently available FIFO space. */
static uint32_t usb_bulk_write_chunk(const uint8_t *data, uint32_t length, uint32_t available) {
    uint32_t chunk;

    if ((data == NULL) || (length == 0u) || !tud_ready() || (available == 0u)) {
        return 0u;
    }

    chunk = (length < available) ? length : available;
    return tud_vendor_n_write(USB_VENDOR_ITF, data, chunk);
}
/** @brief Pull packets from the trace ring and stream them over the vendor endpoint. */
static bool usb_bulk_poll_trace_ring(void) {
    bool wrote_any = false;
    uint32_t available = 0u;
    uint32_t bytes_remaining = USB_BULK_SERVICE_QUANTUM_BYTES;

    while (bytes_remaining > 0u) {
        usb_bulk_stream_chunk_t chunk;
        uint32_t written;

        if (!usb_bulk_prepare_stream_chunk(bytes_remaining, &chunk)) {
            break;
        }

        if (available == 0u) {
            available = tud_vendor_n_write_available(USB_VENDOR_ITF);
        }

        if (available == 0u) {
            g_usb_bulk_stream_state.host_backpressure_stall_count += 1u;
            break;
        }

        written = usb_bulk_write_chunk(chunk.data, chunk.length, available);
        if (written == 0u) {
            break;
        }

        wrote_any = true;
        available -= written;
        bytes_remaining -= written;
        g_usb_bulk_stream_state.packet_offset += written;
        if (g_usb_bulk_stream_state.packet_offset >= g_usb_bulk_stream_state.packet_bytes) {
            trace_ring_pop();
            usb_bulk_reset_stream_state();
        }
    }

    return wrote_any;
}

/** @copydoc usb_bulk_service_stream */
bool usb_bulk_service_stream(bool enabled) {
    if (!enabled) {
        usb_bulk_reset_stream_state();
        return false;
    }

    if (!tud_ready()) {
        return false;
    }

    return usb_bulk_poll_trace_ring();
}

/** @copydoc usb_bulk_flush */
void usb_bulk_flush(void) { tud_vendor_n_write_flush(USB_VENDOR_ITF); }

/** @copydoc usb_bulk_host_backpressure_stall_count */
uint32_t usb_bulk_host_backpressure_stall_count(void) {
    return g_usb_bulk_stream_state.host_backpressure_stall_count;
}