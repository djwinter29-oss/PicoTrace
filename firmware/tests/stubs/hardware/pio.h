#ifndef HARDWARE_PIO_H
#define HARDWARE_PIO_H

#include <stdbool.h>
#include <stdint.h>

typedef unsigned int uint;

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

#define pio1 (&g_stub_pio1)

static inline uint pio_add_program(PIO pio, const pio_program_t *program) {
    static uint next_offset = 0u;

    (void)pio;
    (void)program;
    return next_offset++;
}

static inline void pio_sm_set_enabled(PIO pio, uint sm, bool enabled) {
    (void)pio;
    (void)sm;
    (void)enabled;
}

static inline void pio_sm_clear_fifos(PIO pio, uint sm) {
    (void)pio;
    (void)sm;
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