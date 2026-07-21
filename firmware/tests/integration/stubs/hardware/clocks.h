#ifndef HARDWARE_CLOCKS_H
#define HARDWARE_CLOCKS_H

#include <stdbool.h>
#include <stdint.h>

#include <assert.h>

typedef enum {
    clk_sys = 0,
} clock_index;

static inline bool set_sys_clock_khz(uint32_t freq_khz, bool required) {
    (void)freq_khz;
    (void)required;
    return true;
}

static inline uint32_t clock_get_hz(clock_index clk_index) {
    (void)clk_index;
    return 125000000u;
}

#define hard_assert(condition) assert(condition)

#endif