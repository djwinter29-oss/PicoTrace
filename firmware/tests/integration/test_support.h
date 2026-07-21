#ifndef TEST_SUPPORT_H
#define TEST_SUPPORT_H

#include <stdbool.h>
#include <stdint.h>

void reset_usb_stub(void);
uint32_t test_stub_led_set_calls(void);
bool test_stub_led_state(void);
uint32_t test_stub_watchdog_reboot_calls(void);
uint32_t test_stub_watchdog_reboot_pc(void);
uint32_t test_stub_watchdog_reboot_sp(void);
uint32_t test_stub_watchdog_reboot_delay_ms(void);

#endif