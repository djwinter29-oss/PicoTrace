#include "usb/usb_cdc.h"

#include "tusb.h"

#include <stddef.h>

#define USB_CDC_ITF 0u
#define USB_CDC_PACKET_SIZE 64u
#define USB_CDC_RX_QUEUE_SIZE 128u
#define USB_CDC_TX_QUEUE_SIZE 256u

static uint8_t usb_cdc_rx_queue[USB_CDC_RX_QUEUE_SIZE];
static uint32_t usb_cdc_rx_head;
static uint32_t usb_cdc_rx_tail;
static uint32_t usb_cdc_rx_count;
static uint8_t usb_cdc_tx_queue[USB_CDC_TX_QUEUE_SIZE];
static uint32_t usb_cdc_tx_head;
static uint32_t usb_cdc_tx_tail;
static uint32_t usb_cdc_tx_count;

static void usb_cdc_queue_byte(uint8_t byte) {
    if (usb_cdc_rx_count >= USB_CDC_RX_QUEUE_SIZE) {
        return;
    }

    usb_cdc_rx_queue[usb_cdc_rx_head] = byte;
    usb_cdc_rx_head = (usb_cdc_rx_head + 1u) % USB_CDC_RX_QUEUE_SIZE;
    usb_cdc_rx_count += 1u;
}

bool usb_cdc_is_connected(void) {
    return tud_ready() && tud_cdc_n_connected(USB_CDC_ITF);
}

static bool usb_cdc_tx_queue_push(const uint8_t *data, uint32_t length) {
    if ((data == NULL) || (length == 0u)) {
        return false;
    }

    if ((USB_CDC_TX_QUEUE_SIZE - usb_cdc_tx_count) < length) {
        return false;
    }

    for (uint32_t index = 0u; index < length; ++index) {
        usb_cdc_tx_queue[usb_cdc_tx_head] = data[index];
        usb_cdc_tx_head = (usb_cdc_tx_head + 1u) % USB_CDC_TX_QUEUE_SIZE;
        usb_cdc_tx_count += 1u;
    }

    return true;
}

uint32_t usb_cdc_read(uint8_t *data, uint32_t capacity) {
    uint32_t count = 0u;

    if ((data == NULL) || (capacity == 0u)) {
        return 0u;
    }

    while ((count < capacity) && (usb_cdc_rx_count != 0u)) {
        data[count] = usb_cdc_rx_queue[usb_cdc_rx_tail];
        usb_cdc_rx_tail = (usb_cdc_rx_tail + 1u) % USB_CDC_RX_QUEUE_SIZE;
        usb_cdc_rx_count -= 1u;
        count += 1u;
    }

    return count;
}

bool usb_cdc_write(const uint8_t *data, uint32_t length) {
    if ((data == NULL) || (length == 0u) || !tud_ready()) {
        return false;
    }

    if (!usb_cdc_tx_queue_push(data, length)) {
        return false;
    }

    usb_cdc_poll_tx();
    return true;
}

void usb_cdc_poll_tx(void) {
    bool flushed = false;

    if (!tud_ready()) {
        return;
    }

    while (usb_cdc_tx_count != 0u) {
        uint32_t available = tud_cdc_n_write_available(USB_CDC_ITF);
        uint32_t contiguous = USB_CDC_TX_QUEUE_SIZE - usb_cdc_tx_tail;
        uint32_t chunk = usb_cdc_tx_count;
        uint32_t written;

        if (available == 0u) {
            break;
        }

        if (chunk > contiguous) {
            chunk = contiguous;
        }

        if (chunk > available) {
            chunk = available;
        }

        written = tud_cdc_n_write(USB_CDC_ITF, &usb_cdc_tx_queue[usb_cdc_tx_tail], chunk);
        if (written == 0u) {
            break;
        }

        usb_cdc_tx_tail = (usb_cdc_tx_tail + written) % USB_CDC_TX_QUEUE_SIZE;
        usb_cdc_tx_count -= written;
        flushed = true;
    }

    if (flushed) {
        tud_cdc_n_write_flush(USB_CDC_ITF);
    }
}

void tud_cdc_rx_cb(uint8_t itf) {
    uint8_t buffer[USB_CDC_PACKET_SIZE];

    while (tud_cdc_n_available(itf) != 0u) {
        uint32_t count = tud_cdc_n_read(itf, buffer, sizeof(buffer));

        if (count == 0u) {
            break;
        }

        for (uint32_t index = 0u; index < count; ++index) {
            usb_cdc_queue_byte(buffer[index]);
        }
    }
}

void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts) {
    (void)itf;
    (void)dtr;
    (void)rts;
}