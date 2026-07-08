#ifndef USB_BULK_H
#define USB_BULK_H

#include <stdbool.h>
#include <stdint.h>

bool usb_bulk_write(const uint8_t *data, uint32_t length);
uint32_t usb_bulk_stream_write(const uint8_t *data, uint32_t length);
void usb_bulk_flush(void);

#endif