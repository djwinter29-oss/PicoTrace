#include "app_control_test.h"

#include <assert.h>
#include <string.h>

#include "app_control.h"
#include "test_support.h"

static void test_app_control_init_enables_stream_and_turns_led_off(void) {
    reset_usb_stub();

    app_control_init();

    assert(app_control_stream_enabled() == true);
    assert(test_stub_led_state() == false);
    assert(test_stub_led_set_calls() == 1u);
}

static void test_app_control_set_stream_enabled_updates_state(void) {
    reset_usb_stub();

    app_control_set_stream_enabled(false);
    assert(app_control_stream_enabled() == false);

    app_control_set_stream_enabled(true);
    assert(app_control_stream_enabled() == true);
}

static void test_app_control_set_led_updates_driver_state(void) {
    reset_usb_stub();

    app_control_set_led(true);
    assert(test_stub_led_state() == true);
    assert(test_stub_led_set_calls() == 1u);

    app_control_set_led(false);
    assert(test_stub_led_state() == false);
    assert(test_stub_led_set_calls() == 2u);
}

static void test_app_control_reboot_uses_system_reboot(void) {
    reset_usb_stub();

    app_control_reboot();

    assert(test_stub_watchdog_reboot_calls() == 1u);
    assert(test_stub_watchdog_reboot_pc() == 0u);
    assert(test_stub_watchdog_reboot_sp() == 0u);
    assert(test_stub_watchdog_reboot_delay_ms() == 0u);
}

static void test_app_control_firmware_version_returns_build_time_string(void) {
    reset_usb_stub();

    assert(strcmp(app_control_firmware_version(), "test") == 0);
}

void run_app_control_tests(void) {
    test_app_control_init_enables_stream_and_turns_led_off();
    test_app_control_set_stream_enabled_updates_state();
    test_app_control_set_led_updates_driver_state();
    test_app_control_reboot_uses_system_reboot();
    test_app_control_firmware_version_returns_build_time_string();
}