#ifndef HARDWARE_GPIO_H
#define HARDWARE_GPIO_H

#include <stdbool.h>
#include <stdint.h>

typedef unsigned int uint;

extern uint64_t stub_gpio_high_mask;

#define GPIO_IN false
#define GPIO_OUT true

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

static inline void stub_gpio_set_level(uint gpio, bool high) {
    if (gpio >= 64u) {
        return;
    }

    if (high) {
        stub_gpio_high_mask |= ((uint64_t)1u << gpio);
        return;
    }

    stub_gpio_high_mask &= ~((uint64_t)1u << gpio);
}

static inline bool gpio_get(uint gpio) {
    if (gpio >= 64u) {
        return true;
    }

    return ((stub_gpio_high_mask >> gpio) & 1u) != 0u;
}

#endif