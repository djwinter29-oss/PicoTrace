/**
 * @file app_control.h
 * @brief Shared application-level control state used by USB, CLI, and monitor code.
 */

#ifndef APP_CONTROL_H
#define APP_CONTROL_H

#include <stdbool.h>

/** @brief Initialize the shared application control state. */
void app_control_init(void);

/** @brief Return whether vendor bulk trace streaming is currently enabled. */
bool app_control_stream_enabled(void);

/**
 * @brief Update the shared vendor bulk streaming enable state.
 * @param enabled New stream enable state.
 */
void app_control_set_stream_enabled(bool enabled);

/**
 * @brief Update the shared board LED state.
 * @param on When true, request LED on; otherwise request LED off.
 */
void app_control_set_led(bool on);

/** @brief Request a board reboot through the shared system control path. */
void app_control_reboot(void);

#endif