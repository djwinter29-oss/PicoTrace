#ifndef HARDWARE_DMA_H
#define HARDWARE_DMA_H

#include <stdbool.h>
#include <stdint.h>

typedef unsigned int uint;

typedef enum {
    DMA_SIZE_8 = 0,
    DMA_SIZE_16 = 1,
    DMA_SIZE_32 = 2,
} dma_channel_transfer_size;

typedef struct {
    dma_channel_transfer_size transfer_size;
    bool read_increment;
    bool write_increment;
    uint32_t dreq;
    bool ring_write;
    uint32_t ring_bits;
} dma_channel_config;

typedef struct {
    uintptr_t read_addr;
    uintptr_t write_addr;
    uint32_t transfer_count;
} dma_channel_hw_t;

typedef struct {
    volatile uint32_t ints0;
} dma_hw_t;

extern bool stub_dma_configure_fail_next;

static dma_channel_hw_t g_stub_dma_channels[16];
static bool g_stub_dma_claimed[16];
static dma_hw_t g_stub_dma_hw;

#define dma_hw (&g_stub_dma_hw)

static inline int dma_claim_unused_channel(bool required) {
    (void)required;

    for (int channel = 0; channel < 16; ++channel) {
        if (!g_stub_dma_claimed[channel]) {
            g_stub_dma_claimed[channel] = true;
            return channel;
        }
    }

    return -1;
}

static inline void dma_channel_unclaim(uint channel) {
    g_stub_dma_claimed[channel] = false;
}

static inline dma_channel_config dma_channel_get_default_config(uint channel) {
    dma_channel_config config = {0};

    (void)channel;
    return config;
}

static inline void channel_config_set_transfer_data_size(dma_channel_config *config, dma_channel_transfer_size size) {
    config->transfer_size = size;
}

static inline void channel_config_set_read_increment(dma_channel_config *config, bool enable) {
    config->read_increment = enable;
}

static inline void channel_config_set_write_increment(dma_channel_config *config, bool enable) {
    config->write_increment = enable;
}

static inline void channel_config_set_dreq(dma_channel_config *config, uint32_t dreq) {
    config->dreq = dreq;
}

static inline void channel_config_set_ring(dma_channel_config *config, bool write, uint size_bits) {
    config->ring_write = write;
    config->ring_bits = size_bits;
}

static inline void dma_channel_configure(uint channel,
                                         const dma_channel_config *config,
                                         volatile void *write_addr,
                                         const volatile void *read_addr,
                                         uint32_t transfer_count,
                                         bool trigger) {
    (void)config;
    (void)trigger;

    if (stub_dma_configure_fail_next) {
        stub_dma_configure_fail_next = false;
        g_stub_dma_channels[channel].write_addr = 0u;
        g_stub_dma_channels[channel].read_addr = 0u;
        g_stub_dma_channels[channel].transfer_count = 0u;
        return;
    }

    g_stub_dma_channels[channel].write_addr = (uintptr_t)write_addr;
    g_stub_dma_channels[channel].read_addr = (uintptr_t)read_addr;
    g_stub_dma_channels[channel].transfer_count = transfer_count;
}

static inline void dma_channel_abort(uint channel) {
    (void)channel;
}

static inline void dma_channel_set_write_addr(uint channel, volatile void *write_addr, bool trigger) {
    (void)trigger;
    g_stub_dma_channels[channel].write_addr = (uintptr_t)write_addr;
}

static inline void dma_channel_set_trans_count(uint channel, uint32_t transfer_count, bool trigger) {
    (void)trigger;
    g_stub_dma_channels[channel].transfer_count = transfer_count;
}

static inline void dma_channel_set_irq0_enabled(uint channel, bool enabled) {
    (void)channel;
    (void)enabled;
}

static inline bool dma_channel_is_busy(uint channel) {
    (void)channel;
    return false;
}

static inline dma_channel_hw_t *dma_channel_hw_addr(uint channel) {
    return &g_stub_dma_channels[channel];
}

#endif