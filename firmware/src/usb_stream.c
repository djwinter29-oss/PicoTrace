#include "usb_stream.h"

#include <stddef.h>
#include <string.h>

#include "trace/trace_packet.h"
#include "trace/trace_ring.h"

#define USB_STREAM_FRAME_SIZE 4096u
#define USB_STREAM_PATTERN_SIZE 8u

static const uint8_t usb_stream_pattern[USB_STREAM_PATTERN_SIZE] = {
    'S', 'T', 'R', 'M', 0xA5u, 0x5Au, 0xC3u, 0x3Cu,
};

static uint8_t usb_stream_frame[USB_STREAM_FRAME_SIZE];
static bool usb_stream_frame_initialized;
static const trace_packet_t *usb_stream_packet;
static uint32_t usb_stream_packet_offset;

static void usb_stream_init_frame(void) {
    uint32_t offset;

    if (usb_stream_frame_initialized) {
        return;
    }

    for (offset = 0u; offset < USB_STREAM_FRAME_SIZE; offset += USB_STREAM_PATTERN_SIZE) {
        memcpy(&usb_stream_frame[offset], usb_stream_pattern, USB_STREAM_PATTERN_SIZE);
    }

    usb_stream_frame_initialized = true;
}

static uint32_t usb_stream_packet_bytes(const trace_packet_t *packet) {
    uint32_t payload_len = packet->header.payload_len;

    if (payload_len > TRACE_PACKET_PAYLOAD_BYTES) {
        payload_len = TRACE_PACKET_PAYLOAD_BYTES;
    }

    return TRACE_PACKET_HEADER_BYTES + payload_len;
}

static bool usb_stream_poll_trace_ring(usb_stream_write_t write_frame) {
    bool emitted = false;

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
        }

        emitted = true;
        packet_bytes = usb_stream_packet_bytes(usb_stream_packet);
        remaining = packet_bytes - usb_stream_packet_offset;
        written = write_frame(((const uint8_t *)usb_stream_packet) + usb_stream_packet_offset, remaining);
        if (written == 0u) {
            break;
        }

        usb_stream_packet_offset += written;
        if (usb_stream_packet_offset >= packet_bytes) {
            trace_ring_pop();
            usb_stream_packet = NULL;
            usb_stream_packet_offset = 0u;
        }
    }

    return emitted;
}

void usb_stream_poll(bool enabled, usb_stream_write_t write_frame) {
    static uint32_t usb_stream_offset;

    if (!enabled || (write_frame == NULL)) {
        return;
    }

    if (usb_stream_poll_trace_ring(write_frame)) {
        return;
    }

    usb_stream_init_frame();

    while (true) {
        uint32_t contiguous = USB_STREAM_FRAME_SIZE - usb_stream_offset;
        uint32_t written = write_frame(&usb_stream_frame[usb_stream_offset], contiguous);

        if (written == 0u) {
            break;
        }

        usb_stream_offset = (usb_stream_offset + written) % USB_STREAM_FRAME_SIZE;
    }
}