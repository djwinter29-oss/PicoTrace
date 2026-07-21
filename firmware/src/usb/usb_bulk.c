/**
 * @file usb_bulk.c
 * @brief TinyUSB vendor bulk helpers for trace-ring streaming and bounded writes.
 */

#include "usb/usb_bulk.h"

#include "tusb.h"

#include <stddef.h>

#include "trace/trace_packet.h"
#include "trace/trace_ring.h"

/** @brief TinyUSB vendor interface index used for trace streaming. */
#define USB_VENDOR_ITF 0u
/** @brief Vendor endpoint packet size used when streaming trace data. */
#define USB_VENDOR_PACKET_SIZE 64u
/** @brief Upper bound on vendor stream bytes queued per service pass to keep control paths responsive. */
#define USB_VENDOR_STREAM_WRITE_BUDGET_BYTES 1024u

/** @brief Stream-side packet and observability state retained across vendor drain polls. */
typedef struct {
    const trace_packet_t *packet; /**< Borrowed pointer to the trace packet currently being streamed. */
    uint32_t packet_offset; /**< Byte offset already transmitted from @ref packet. */
    uint32_t packet_bytes; /**< Cached total byte length for @ref packet. */
    uint32_t host_backpressure_stall_count; /**< Number of polls that found queued data but no vendor write space. */
    uint32_t policy_deferral_count; /**< Number of stream writes that deferred bytes because of stream write policy. */
    uint32_t deferred_bytes_count; /**< Number of bytes deferred by stream packet-alignment policy. */
} usb_bulk_stream_state_t;

/** @brief Persistent vendor stream state. */
static usb_bulk_stream_state_t g_usb_bulk_stream_state;

/** @brief Prepared view of the next consumer-side USB write span. */
typedef struct {
    const uint8_t *data; /**< Start of the current borrowed packet span to write. */
    uint32_t length; /**< Maximum bytes worth attempting to write for this span. */
    bool completes_packet; /**< Indicates whether writing @ref length bytes would finish the current logical packet. */
} usb_bulk_stream_chunk_t;

/** @brief Per-pass accounting for stream-write policy deferrals. */
typedef struct {
    uint32_t policy_deferrals; /**< Number of writes trimmed by the stream chunk policy in the current pass. */
    uint32_t deferred_bytes; /**< Number of bytes deferred by the stream chunk policy in the current pass. */
} usb_bulk_stream_write_stats_t;

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

        if (packet == NULL) {
            return false;
        }

        if (packet->header.payload_len > TRACE_PACKET_PAYLOAD_BYTES) {
            trace_ring_pop();
            continue;
        }

        g_usb_bulk_stream_state.packet = packet;
        g_usb_bulk_stream_state.packet_offset = 0u;
        g_usb_bulk_stream_state.packet_bytes = TRACE_PACKET_HEADER_BYTES + packet->header.payload_len;
    }

    chunk_out->data = ((const uint8_t *)g_usb_bulk_stream_state.packet) + g_usb_bulk_stream_state.packet_offset;
    chunk_out->length = g_usb_bulk_stream_state.packet_bytes - g_usb_bulk_stream_state.packet_offset;
    if (chunk_out->length > bytes_remaining) {
        chunk_out->length = bytes_remaining;
    }
    chunk_out->completes_packet =
        ((g_usb_bulk_stream_state.packet_offset + chunk_out->length) == g_usb_bulk_stream_state.packet_bytes);
    return true;
}

/** @brief Attempt one vendor bulk stream write using the current alignment policy. */
static uint32_t usb_bulk_write_chunk(
    const uint8_t *data,
    uint32_t length,
    uint32_t available,
    bool completes_packet,
    usb_bulk_stream_write_stats_t *stats
) {
    uint32_t chunk;
    uint32_t trimmed_chunk;

    if ((data == NULL) || (length == 0u) || !tud_ready() || (available == 0u)) {
        return 0u;
    }

    chunk = (length < available) ? length : available;
    trimmed_chunk = chunk;
    if ((trimmed_chunk > USB_VENDOR_PACKET_SIZE) && !(completes_packet && (chunk == length))) {
        trimmed_chunk -= trimmed_chunk % USB_VENDOR_PACKET_SIZE;
    }

    if (trimmed_chunk == 0u) {
        return 0u;
    }

    if ((trimmed_chunk < chunk) && (stats != NULL)) {
        stats->policy_deferrals += 1u;
        stats->deferred_bytes += chunk - trimmed_chunk;
    }

    return tud_vendor_n_write(USB_VENDOR_ITF, data, trimmed_chunk);
}
/** @brief Pull packets from the trace ring and stream them over the vendor endpoint. */
static bool usb_bulk_poll_trace_ring(void) {
    bool wrote_any = false;
    uint32_t available = 0u;
    uint32_t bytes_remaining = USB_VENDOR_STREAM_WRITE_BUDGET_BYTES;
    usb_bulk_stream_write_stats_t stats = {0};

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

        written = usb_bulk_write_chunk(
            chunk.data,
            chunk.length,
            available,
            chunk.completes_packet,
            &stats
        );
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

    g_usb_bulk_stream_state.policy_deferral_count += stats.policy_deferrals;
    g_usb_bulk_stream_state.deferred_bytes_count += stats.deferred_bytes;

    return wrote_any;
}

/** @copydoc usb_bulk_write */
bool usb_bulk_write(const uint8_t *data, uint32_t length) {
    uint32_t available;

    if (!tud_ready()) {
        return false;
    }

    available = tud_vendor_n_write_available(USB_VENDOR_ITF);
    if ((data == NULL) || (length == 0u) || (available < length)) {
        return false;
    }

    return tud_vendor_n_write(USB_VENDOR_ITF, data, length) == length;
}

/** @copydoc usb_bulk_stream_write */
uint32_t usb_bulk_stream_write(const uint8_t *data, uint32_t length) {
    usb_bulk_stream_write_stats_t stats = {0};
    uint32_t written;

    if (!tud_ready()) {
        return 0u;
    }

    written = usb_bulk_write_chunk(data, length, tud_vendor_n_write_available(USB_VENDOR_ITF), false, &stats);
    g_usb_bulk_stream_state.policy_deferral_count += stats.policy_deferrals;
    g_usb_bulk_stream_state.deferred_bytes_count += stats.deferred_bytes;
    return written;
}

/** @copydoc usb_bulk_service_stream */
bool usb_bulk_service_stream(bool enabled) {
    bool wrote_any;

    if (!enabled) {
        usb_bulk_reset_stream_state();
        return false;
    }

    if (!tud_ready()) {
        return false;
    }

    wrote_any = usb_bulk_poll_trace_ring();
    if (wrote_any) {
        tud_vendor_n_write_flush(USB_VENDOR_ITF);
    }
    return wrote_any;
}

/** @copydoc usb_bulk_flush */
void usb_bulk_flush(void) { tud_vendor_n_write_flush(USB_VENDOR_ITF); }

/** @copydoc usb_bulk_host_backpressure_stall_count */
uint32_t usb_bulk_host_backpressure_stall_count(void) {
    return g_usb_bulk_stream_state.host_backpressure_stall_count;
}

/** @copydoc usb_bulk_policy_deferral_count */
uint32_t usb_bulk_policy_deferral_count(void) {
    return g_usb_bulk_stream_state.policy_deferral_count;
}

/** @copydoc usb_bulk_deferred_bytes_count */
uint32_t usb_bulk_deferred_bytes_count(void) {
    return g_usb_bulk_stream_state.deferred_bytes_count;
}