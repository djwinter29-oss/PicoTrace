#ifndef TUSB_H
#define TUSB_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
	HID_REPORT_TYPE_INVALID = 0,
	HID_REPORT_TYPE_INPUT = 1,
	HID_REPORT_TYPE_OUTPUT = 2,
	HID_REPORT_TYPE_FEATURE = 3,
} hid_report_type_t;

bool tud_ready(void);
bool tud_cdc_n_connected(uint8_t itf);
uint32_t tud_cdc_n_write_available(uint8_t itf);
uint32_t tud_cdc_n_write(uint8_t itf, const void *buffer, uint32_t size);
void tud_cdc_n_write_flush(uint8_t itf);
uint32_t tud_cdc_n_available(uint8_t itf);
uint32_t tud_cdc_n_read(uint8_t itf, void *buffer, uint32_t size);
void tud_cdc_rx_cb(uint8_t itf);
void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts);
uint32_t tud_vendor_n_write_available(uint8_t itf);
uint32_t tud_vendor_n_write(uint8_t itf, const void *buffer, uint32_t size);
void tud_vendor_n_write_flush(uint8_t itf);
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t *buffer, uint16_t reqlen);
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const *buffer, uint16_t bufsize);

#endif