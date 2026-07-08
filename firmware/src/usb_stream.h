#ifndef USB_STREAM_H
#define USB_STREAM_H

#include <stdbool.h>
#include <stdint.h>

typedef uint32_t (*usb_stream_write_t)(const uint8_t *data, uint32_t length);

void usb_stream_poll(bool enabled, usb_stream_write_t write_frame);

#endif