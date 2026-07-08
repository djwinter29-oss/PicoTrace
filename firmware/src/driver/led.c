/**
 * @file led.c
 * @brief Board LED helpers for PicoTrace.
 */

#include "pico/stdlib.h"

#include "driver/led.h"

#include <stdbool.h>

/** @copydoc led_init */
void led_init(void) {
#ifdef PICO_DEFAULT_LED_PIN
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_LED_PIN, 0);
#endif
}

/** @copydoc led_set */
void led_set(bool on) {
#ifdef PICO_DEFAULT_LED_PIN
    gpio_put(PICO_DEFAULT_LED_PIN, on ? 1 : 0);
#else
    (void)on;
#endif
}