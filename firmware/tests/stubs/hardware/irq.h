#ifndef HARDWARE_IRQ_H
#define HARDWARE_IRQ_H

#include <stdbool.h>
#include <stdint.h>

typedef unsigned int uint;
typedef void (*irq_handler_t)(void);

enum {
    DMA_IRQ_0 = 0u,
};

static irq_handler_t g_stub_dma_irq0_handler;
static bool g_stub_dma_irq0_enabled;

static inline void irq_set_exclusive_handler(uint irq_num, irq_handler_t handler) {
    if (irq_num == DMA_IRQ_0) {
        g_stub_dma_irq0_handler = handler;
    }
}

static inline void irq_set_enabled(uint irq_num, bool enabled) {
    if (irq_num == DMA_IRQ_0) {
        g_stub_dma_irq0_enabled = enabled;
    }
}

#endif