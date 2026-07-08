#ifndef HARDWARE_CLOCKS_H
#define HARDWARE_CLOCKS_H

#include <stdbool.h>
#include <stdint.h>

#include <assert.h>

static inline bool set_sys_clock_khz(uint32_t freq_khz, bool required) {
    (void)freq_khz;
    (void)required;
    return true;
}

#define hard_assert(condition) assert(condition)

#endif