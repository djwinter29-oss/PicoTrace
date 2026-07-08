#ifndef HARDWARE_PIO_H
#define HARDWARE_PIO_H

#include <stdint.h>

typedef unsigned int uint;
typedef void *PIO;

typedef struct {
    uint16_t instructions[1];
    uint8_t length;
    int8_t origin;
} pio_program_t;

#define pio1 ((PIO)1)

static inline uint pio_add_program(PIO pio, const pio_program_t *program) {
    static uint next_offset = 0u;

    (void)pio;
    (void)program;
    return next_offset++;
}

#endif