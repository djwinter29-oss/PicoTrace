#ifndef PICO_TIME_H
#define PICO_TIME_H

#include <stdint.h>

extern uint32_t stub_time_us32;

static inline uint32_t time_us_32(void) {
    return stub_time_us32;
}

#endif