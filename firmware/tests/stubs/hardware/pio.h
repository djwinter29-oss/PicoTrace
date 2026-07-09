#ifndef HARDWARE_PIO_H
#define HARDWARE_PIO_H

#include <stdbool.h>
#include <stdint.h>

typedef unsigned int uint;

typedef struct {
    uint pin_base;
    bool shift_right;
    bool autopush;
    uint push_threshold;
    float clkdiv;
} pio_sm_config;

typedef struct {
    volatile uint32_t rxf[4];
} pio_hw_t;

typedef pio_hw_t *PIO;

typedef struct {
    const uint16_t *instructions;
    uint8_t length;
    int8_t origin;
} pio_program_t;

static pio_hw_t g_stub_pio1;
static pio_hw_t g_stub_pio0;

#define pio0 (&g_stub_pio0)
#define pio1 (&g_stub_pio1)

static inline bool pio_can_add_program(PIO pio, const pio_program_t *program) {
    (void)pio;
    (void)program;
    return true;
}

static inline uint pio_add_program(PIO pio, const pio_program_t *program) {
    static uint next_offset = 0u;

    (void)pio;
    (void)program;
    return next_offset++;
}

static inline void pio_remove_program(PIO pio, const pio_program_t *program, uint offset) {
    (void)pio;
    (void)program;
    (void)offset;
}

static inline void pio_sm_set_enabled(PIO pio, uint sm, bool enabled) {
    (void)pio;
    (void)sm;
    (void)enabled;
}

static inline void pio_sm_restart(PIO pio, uint sm) {
    (void)pio;
    (void)sm;
}

static inline void pio_sm_clear_fifos(PIO pio, uint sm) {
    (void)pio;
    (void)sm;
}

static inline void pio_gpio_init(PIO pio, uint gpio) {
    (void)pio;
    (void)gpio;
}

static inline void pio_sm_set_consecutive_pindirs(PIO pio, uint sm, uint pin_base, uint count, bool is_out) {
    (void)pio;
    (void)sm;
    (void)pin_base;
    (void)count;
    (void)is_out;
}

static inline void pio_sm_init(PIO pio, uint sm, uint offset, const pio_sm_config *config) {
    (void)pio;
    (void)sm;
    (void)offset;
    (void)config;
}

static inline void sm_config_set_in_pins(pio_sm_config *config, uint pin_base) {
    config->pin_base = pin_base;
}

static inline void sm_config_set_in_shift(pio_sm_config *config, bool shift_right, bool autopush, uint push_threshold) {
    config->shift_right = shift_right;
    config->autopush = autopush;
    config->push_threshold = push_threshold;
}

static inline void sm_config_set_clkdiv(pio_sm_config *config, float clkdiv) {
    config->clkdiv = clkdiv;
}

static inline uint pio_get_dreq(PIO pio, uint sm, bool is_tx) {
    (void)pio;
    (void)is_tx;
    return sm;
}

static inline uint16_t pio_encode_wait_gpio(bool polarity, uint gpio) {
    (void)polarity;
    (void)gpio;
    return 0u;
}

#endif