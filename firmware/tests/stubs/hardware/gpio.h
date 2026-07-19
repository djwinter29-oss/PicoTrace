#ifndef HARDWARE_GPIO_H
#define HARDWARE_GPIO_H

#include <stdbool.h>
#include <stdint.h>

typedef unsigned int uint;
typedef void (*gpio_irq_callback_t)(uint gpio, uint32_t events);

extern uint64_t stub_gpio_high_mask;

#define GPIO_IN false
#define GPIO_OUT true
#define GPIO_IRQ_EDGE_RISE 0x8u

static gpio_irq_callback_t g_stub_gpio_irq_callback;
static uint32_t g_stub_gpio_irq_event_masks[64];

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

static inline void gpio_set_irq_callback(gpio_irq_callback_t callback) {
    g_stub_gpio_irq_callback = callback;
}

static inline void gpio_set_irq_enabled(uint gpio, uint32_t event_mask, bool enabled) {
    if (gpio >= 64u) {
        return;
    }

    if (enabled) {
        g_stub_gpio_irq_event_masks[gpio] |= event_mask;
        return;
    }

    g_stub_gpio_irq_event_masks[gpio] &= ~event_mask;
}

static inline void gpio_set_irq_enabled_with_callback(uint gpio, uint32_t event_mask, bool enabled, gpio_irq_callback_t callback) {
    gpio_set_irq_callback(callback);
    gpio_set_irq_enabled(gpio, event_mask, enabled);
}

static inline void stub_gpio_fire_irq(uint gpio, uint32_t events) {
    if ((gpio >= 64u) || (g_stub_gpio_irq_callback == NULL)) {
        return;
    }

    events &= g_stub_gpio_irq_event_masks[gpio];
    if (events != 0u) {
        g_stub_gpio_irq_callback(gpio, events);
    }
}

#endif