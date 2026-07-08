#include "usb/usb_bulk.h"

#include "tusb.h"

#include <stddef.h>

#include "trace/trace_packet.h"
#include "trace/trace_ring.h"

#define USB_VENDOR_ITF 0u
#define USB_VENDOR_PACKET_SIZE 64u
static const trace_packet_t *usb_stream_packet;
static uint32_t usb_stream_packet_offset;

static void usb_bulk_reset_stream_state(void) {
    usb_stream_packet = NULL;
    usb_stream_packet_offset = 0u;
}

static void usb_bulk_poll_trace_ring(void) {
    while (true) {
        uint32_t packet_bytes;
        uint32_t remaining;
        uint32_t written;

        if (usb_stream_packet == NULL) {
            usb_stream_packet = trace_ring_peek();
            usb_stream_packet_offset = 0u;
            if (usb_stream_packet == NULL) {
                break;
            }

            if (usb_stream_packet->header.payload_len > TRACE_PACKET_PAYLOAD_BYTES) {
                trace_ring_pop();
                usb_bulk_reset_stream_state();
                continue;
            }

            packet_bytes = TRACE_PACKET_HEADER_BYTES + usb_stream_packet->header.payload_len;
        } else {
            packet_bytes = TRACE_PACKET_HEADER_BYTES + usb_stream_packet->header.payload_len;
        }

        remaining = packet_bytes - usb_stream_packet_offset;
        written = usb_bulk_stream_write(((const uint8_t *)usb_stream_packet) + usb_stream_packet_offset, remaining);
        if (written == 0u) {
            break;
        }

        usb_stream_packet_offset += written;
        if (usb_stream_packet_offset >= packet_bytes) {
            trace_ring_pop();
            usb_bulk_reset_stream_state();
        }
    }
}

static uint32_t usb_bulk_write_chunk(const uint8_t *data, uint32_t length, bool exact) {
    uint32_t available;
    uint32_t chunk;
    uint32_t written;

    if ((data == NULL) || (length == 0u) || !tud_ready()) {
        return 0u;
    }

    available = tud_vendor_n_write_available(USB_VENDOR_ITF);
    if (available == 0u) {
        return 0u;
    }

    if (exact && (available < length)) {
        return 0u;
    }

    chunk = length;
    if (chunk > available) {
        chunk = available;
    }

    if (!exact && (chunk > USB_VENDOR_PACKET_SIZE)) {
        chunk -= chunk % USB_VENDOR_PACKET_SIZE;
    }

    if (chunk == 0u) {
        return 0u;
    }

    written = tud_vendor_n_write(USB_VENDOR_ITF, data, chunk);
    return written;
}

bool usb_bulk_write(const uint8_t *data, uint32_t length) {
    return usb_bulk_write_chunk(data, length, true) == length;
}

uint32_t usb_bulk_stream_write(const uint8_t *data, uint32_t length) {
    return usb_bulk_write_chunk(data, length, false);
}

void usb_bulk_poll_stream(bool enabled) {
    if (!enabled) {
        usb_bulk_reset_stream_state();
        return;
    }

    if (!tud_ready()) {
        return;
    }

    usb_bulk_poll_trace_ring();
}

void usb_bulk_flush(void) {
    if (!tud_ready()) {
        return;
    }

    tud_vendor_n_write_flush(USB_VENDOR_ITF);
}