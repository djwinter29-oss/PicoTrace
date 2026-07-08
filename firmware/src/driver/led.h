/**
 * @file led.h
 * @brief Board LED helpers used by the PicoTrace status path.
 */

#ifndef LED_H
#define LED_H

#include <stdbool.h>

/** @brief Initialize the default board LED GPIO when one is available. */
void led_init(void);

/**
 * @brief Set the default board LED state when one is available.
 * @param on When true, request LED on; otherwise request LED off.
 */
void led_set(bool on);

#endif