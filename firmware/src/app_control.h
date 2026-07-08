#ifndef APP_CONTROL_H
#define APP_CONTROL_H

#include <stdbool.h>

void app_control_init(void);
bool app_control_stream_enabled(void);
void app_control_set_stream_enabled(bool enabled);
void app_control_set_led(bool on);
void app_control_reboot(void);

#endif