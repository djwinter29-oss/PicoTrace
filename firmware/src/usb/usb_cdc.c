/**
 * @file usb_cdc.c
 * @brief TinyUSB CDC buffering helpers for the board-local CLI transport.
 */

#include "usb/usb_cdc.h"

#include "tusb.h"

#include <stddef.h>

/** @brief TinyUSB CDC interface index used by the CLI transport. */
#define USB_CDC_ITF 0u
/** @brief Maximum bytes read per TinyUSB CDC receive callback chunk. */
#define USB_CDC_PACKET_SIZE 64u
/** @brief Local receive queue capacity in bytes. */
#define USB_CDC_RX_QUEUE_SIZE 128u
/** @brief Local transmit queue capacity in bytes. */
#define USB_CDC_TX_QUEUE_SIZE 256u

/** @brief Circular receive queue storage. */
static uint8_t usb_cdc_rx_queue[USB_CDC_RX_QUEUE_SIZE];
/** @brief Head index for the receive queue. */
static uint32_t usb_cdc_rx_head;
/** @brief Tail index for the receive queue. */
static uint32_t usb_cdc_rx_tail;
/** @brief Number of bytes currently stored in the receive queue. */
static uint32_t usb_cdc_rx_count;
/** @brief Circular transmit queue storage. */
static uint8_t usb_cdc_tx_queue[USB_CDC_TX_QUEUE_SIZE];
/** @brief Head index for the transmit queue. */
static uint32_t usb_cdc_tx_head;
/** @brief Tail index for the transmit queue. */
static uint32_t usb_cdc_tx_tail;
/** @brief Number of bytes currently stored in the transmit queue. */
static uint32_t usb_cdc_tx_count;

/** @brief Queue one received CDC byte when local receive space remains. */
static void usb_cdc_queue_byte(uint8_t byte) {
    if (usb_cdc_rx_count >= USB_CDC_RX_QUEUE_SIZE) {
        return;
    }

    usb_cdc_rx_queue[usb_cdc_rx_head] = byte;
    usb_cdc_rx_head = (usb_cdc_rx_head + 1u) % USB_CDC_RX_QUEUE_SIZE;
    usb_cdc_rx_count += 1u;
}

/** @copydoc usb_cdc_is_connected */
bool usb_cdc_is_connected(void) {
    return tud_ready() && tud_cdc_n_connected(USB_CDC_ITF);
}

/**
 * @brief Append bytes to the local CDC transmit queue.
 * @param data Caller-owned payload bytes.
 * @param length Number of bytes to queue.
 * @return `true` when all bytes were queued, otherwise `false`.
 */
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

/** @copydoc usb_cdc_read */
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

/** @copydoc usb_cdc_write */
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

/** @copydoc usb_cdc_poll_tx */
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

/**
 * @brief TinyUSB callback used to drain received CDC bytes into the local queue.
 * @param itf TinyUSB CDC interface index that received data.
 */
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

/**
 * @brief TinyUSB callback for CDC line-state changes.
 * @param itf TinyUSB CDC interface index.
 * @param dtr Host DTR state.
 * @param rts Host RTS state.
 */
void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts) {
    (void)itf;
    (void)dtr;
    (void)rts;
}