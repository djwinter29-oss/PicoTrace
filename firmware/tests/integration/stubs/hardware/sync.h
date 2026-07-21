#ifndef HARDWARE_SYNC_H
#define HARDWARE_SYNC_H

#include <stdint.h>

static inline uint32_t save_and_disable_interrupts(void) {
    return 0u;
}

static inline void restore_interrupts(uint32_t irq_state) {
    (void)irq_state;
}

#endif