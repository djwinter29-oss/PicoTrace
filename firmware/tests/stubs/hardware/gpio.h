#ifndef HARDWARE_GPIO_H
#define HARDWARE_GPIO_H

#include <stdbool.h>
#include <stdint.h>

typedef unsigned int uint;

static inline void gpio_init(uint gpio) {
    (void)gpio;
}

static inline void gpio_set_dir(uint gpio, bool out) {
    (void)gpio;
    (void)out;
}

static inline void gpio_disable_pulls(uint gpio) {
    (void)gpio;
}

static inline void gpio_set_input_enabled(uint gpio, bool enabled) {
    (void)gpio;
    (void)enabled;
}

static inline bool gpio_get(uint gpio) {
    (void)gpio;
    return true;
}

#endif