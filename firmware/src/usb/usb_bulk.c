#include "usb/usb_bulk.h"

#include "tusb.h"

#include <stddef.h>

#define USB_VENDOR_ITF 0u
#define USB_VENDOR_PACKET_SIZE 64u

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

void usb_bulk_flush(void) {
    if (!tud_ready()) {
        return;
    }

    tud_vendor_n_write_flush(USB_VENDOR_ITF);
}