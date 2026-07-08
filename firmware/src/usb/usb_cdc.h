#ifndef USB_CDC_H
#define USB_CDC_H

#include <stdbool.h>
#include <stdint.h>

bool usb_cdc_is_connected(void);
uint32_t usb_cdc_read(uint8_t *data, uint32_t capacity);
bool usb_cdc_write(const uint8_t *data, uint32_t length);
void usb_cdc_poll_tx(void);

#endif