/**
 * @file usb_cdc.c
 * @brief TinyUSB CDC buffering helpers for the board-local CLI transport.
 */

#include "usb/usb_cdc.h"

#include "tusb.h"

#include <stddef.h>
#include <string.h>

/** @brief TinyUSB CDC interface index used by the CLI transport. */
#define USB_CDC_ITF 0u
/** @brief Maximum bytes read per TinyUSB CDC receive callback chunk. */
#define USB_CDC_PACKET_SIZE 64u
/** @brief Maximum TinyUSB CDC packets drained in one receive callback invocation. */
#define USB_CDC_RX_CB_MAX_PACKETS 2u
/** @brief Maximum CDC bytes flushed in one poll pass to avoid monopolizing loop time. */
#define USB_CDC_TX_POLL_MAX_BYTES (USB_CDC_PACKET_SIZE * 2u)
/** @brief Local receive queue capacity in bytes. */
#define USB_CDC_RX_QUEUE_SIZE 512u
/** @brief Local transmit queue capacity in bytes. */
#define USB_CDC_TX_QUEUE_SIZE 1024u

/** @brief Ring indices and occupancy for one local CDC byte queue. */
typedef struct {
    uint32_t head; /**< Next write position in the ring. */
    uint32_t tail; /**< Next read position in the ring. */
    uint32_t count; /**< Number of queued bytes currently stored in the ring. */
} usb_cdc_ring_state_t;

/** @brief Circular receive queue storage. */
static uint8_t usb_cdc_rx_queue[USB_CDC_RX_QUEUE_SIZE];
/** @brief Ring indices and occupancy for the receive queue. */
static usb_cdc_ring_state_t usb_cdc_rx_ring;
/** @brief Circular transmit queue storage. */
static uint8_t usb_cdc_tx_queue[USB_CDC_TX_QUEUE_SIZE];
/** @brief Ring indices and occupancy for the transmit queue. */
static usb_cdc_ring_state_t usb_cdc_tx_ring;
/** @brief Lightweight observability counters for CDC control-path behavior. */
static usb_cdc_stats_t usb_cdc_stats;

/* Ring mechanics */

/** @brief Return free space in one local CDC byte ring. */
static uint32_t usb_cdc_ring_free_space(const usb_cdc_ring_state_t *ring, uint32_t capacity) {
    return capacity - ring->count;
}

/** @brief Append bytes to one local CDC byte ring until either data or free space is exhausted. */
static uint32_t usb_cdc_ring_write(
    uint8_t *buffer,
    uint32_t capacity,
    usb_cdc_ring_state_t *ring,
    const uint8_t *data,
    uint32_t length
) {
    uint32_t written = 0u;

    if ((buffer == NULL) || (ring == NULL) || (data == NULL) || (length == 0u)) {
        return 0u;
    }

    if (length > usb_cdc_ring_free_space(ring, capacity)) {
        length = usb_cdc_ring_free_space(ring, capacity);
    }

    while (written < length) {
        uint32_t contiguous = capacity - ring->head;
        uint32_t chunk = length - written;

        if (chunk > contiguous) {
            chunk = contiguous;
        }

        memcpy(&buffer[ring->head], &data[written], chunk);
        ring->head = (ring->head + chunk) % capacity;
        ring->count += chunk;
        written += chunk;
    }

    return written;
}

/** @brief Remove queued bytes from one local CDC byte ring into caller-owned storage. */
static uint32_t usb_cdc_ring_read(
    const uint8_t *buffer,
    uint32_t capacity,
    usb_cdc_ring_state_t *ring,
    uint8_t *data,
    uint32_t length
) {
    uint32_t read_count = 0u;

    if ((buffer == NULL) || (ring == NULL) || (data == NULL) || (length == 0u)) {
        return 0u;
    }

    if (length > ring->count) {
        length = ring->count;
    }

    while (read_count < length) {
        uint32_t contiguous = capacity - ring->tail;
        uint32_t chunk = length - read_count;

        if (chunk > contiguous) {
            chunk = contiguous;
        }

        memcpy(&data[read_count], &buffer[ring->tail], chunk);
        ring->tail = (ring->tail + chunk) % capacity;
        ring->count -= chunk;
        read_count += chunk;
    }

    return read_count;
}

/** @brief Queue as many received CDC bytes as local receive space allows. */
static uint32_t usb_cdc_rx_queue_push(const uint8_t *data, uint32_t length) {
    uint32_t dropped = 0u;

    if ((data == NULL) || (length == 0u)) {
        return 0u;
    }

    if (length > usb_cdc_ring_free_space(&usb_cdc_rx_ring, USB_CDC_RX_QUEUE_SIZE)) {
        dropped = length - usb_cdc_ring_free_space(&usb_cdc_rx_ring, USB_CDC_RX_QUEUE_SIZE);
    }

    usb_cdc_stats.rx_dropped_bytes += dropped;
    return usb_cdc_ring_write(usb_cdc_rx_queue, USB_CDC_RX_QUEUE_SIZE, &usb_cdc_rx_ring, data, length);
}

/** @brief Return currently available space in the local receive queue. */
static uint32_t usb_cdc_rx_queue_free_space(void) {
    return usb_cdc_ring_free_space(&usb_cdc_rx_ring, USB_CDC_RX_QUEUE_SIZE);
}

/** @copydoc usb_cdc_is_connected */
bool usb_cdc_is_connected(void) {
    return tud_ready() && tud_cdc_n_connected(USB_CDC_ITF);
}

/* USB policy */

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

    if (usb_cdc_ring_free_space(&usb_cdc_tx_ring, USB_CDC_TX_QUEUE_SIZE) < length) {
        return false;
    }

    return usb_cdc_ring_write(usb_cdc_tx_queue, USB_CDC_TX_QUEUE_SIZE, &usb_cdc_tx_ring, data, length) == length;
}

/** @copydoc usb_cdc_read */
uint32_t usb_cdc_read(uint8_t *data, uint32_t capacity) {
    if ((data == NULL) || (capacity == 0u)) {
        return 0u;
    }

    return usb_cdc_ring_read(usb_cdc_rx_queue, USB_CDC_RX_QUEUE_SIZE, &usb_cdc_rx_ring, data, capacity);
}

/** @copydoc usb_cdc_write */
bool usb_cdc_write(const uint8_t *data, uint32_t length) {
    if ((data == NULL) || (length == 0u) || !tud_ready()) {
        return false;
    }

    if (!usb_cdc_tx_queue_push(data, length)) {
        /* Try one immediate flush pass before giving up when bursts momentarily fill the queue. */
        usb_cdc_poll_tx();
        if (!usb_cdc_tx_queue_push(data, length)) {
            usb_cdc_stats.tx_enqueue_failures += 1u;
            return false;
        }
    }

    usb_cdc_poll_tx();
    return true;
}

/** @copydoc usb_cdc_poll_tx */
void usb_cdc_poll_tx(void) {
    uint32_t budget_remaining = USB_CDC_TX_POLL_MAX_BYTES;
    bool flushed = false;

    if (!tud_ready()) {
        return;
    }

    while ((usb_cdc_tx_ring.count != 0u) && (budget_remaining != 0u)) {
        uint32_t available = tud_cdc_n_write_available(USB_CDC_ITF);
        uint32_t contiguous = USB_CDC_TX_QUEUE_SIZE - usb_cdc_tx_ring.tail;
        uint32_t chunk = usb_cdc_tx_ring.count;
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

        if (chunk > budget_remaining) {
            chunk = budget_remaining;
        }

        written = tud_cdc_n_write(USB_CDC_ITF, &usb_cdc_tx_queue[usb_cdc_tx_ring.tail], chunk);
        if (written == 0u) {
            break;
        }

        usb_cdc_tx_ring.tail = (usb_cdc_tx_ring.tail + written) % USB_CDC_TX_QUEUE_SIZE;
        usb_cdc_tx_ring.count -= written;
        budget_remaining -= written;
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
    uint32_t packets_drained = 0u;

    while ((tud_cdc_n_available(itf) != 0u) && (packets_drained < USB_CDC_RX_CB_MAX_PACKETS)) {
        uint32_t free_space = usb_cdc_rx_queue_free_space();
        uint32_t count;

        if (free_space == 0u) {
            break;
        }

        if (free_space > sizeof(buffer)) {
            free_space = sizeof(buffer);
        }

        count = tud_cdc_n_read(itf, buffer, free_space);

        if (count == 0u) {
            break;
        }

        (void)usb_cdc_rx_queue_push(buffer, count);
        packets_drained += 1u;
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

usb_cdc_stats_t usb_cdc_get_stats(void) {
    return usb_cdc_stats;
}

void usb_cdc_reset_stats(void) {
    memset(&usb_cdc_stats, 0, sizeof(usb_cdc_stats));
}