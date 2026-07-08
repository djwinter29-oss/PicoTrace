#include "app_control.h"

#include "driver/led.h"
#include "driver/system.h"

static volatile bool app_control_stream_on = true;
static bool app_control_led_on;

void app_control_init(void) {
    app_control_stream_on = true;
    app_control_led_on = false;
    led_set(false);
}

bool app_control_stream_enabled(void) {
    return app_control_stream_on;
}

void app_control_set_stream_enabled(bool enabled) {
    app_control_stream_on = enabled;
}

void app_control_set_led(bool on) {
    app_control_led_on = on;
    led_set(on);
}

void app_control_reboot(void) {
    system_reboot();
}