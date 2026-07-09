#include "i2c_monitor_test_hooks.h"

#include <string.h>

#include "trace/capture/i2c_monitor.c"

void i2c_monitor_test_reset(void) {
    if (g_i2c_monitor_initialized) {
        i2c_monitor_shutdown_all();
    }

    g_i2c_monitor_initialized = false;
    g_i2c_monitor_init_failed = false;
    g_i2c_monitor_program_offset = 0u;
}

bool i2c_monitor_test_start_channel(uint32_t channel, uint32_t sample_hz) {
    if (i2c_monitor_init() != I2C_MONITOR_RC_OK) {
        return false;
    }

    return i2c_monitor_set_channel_sample_hz(channel, sample_hz) == I2C_MONITOR_RC_OK;
}

bool i2c_monitor_test_feed_completed_buffer(
    uint32_t channel,
    const uint32_t *raw_words,
    uint32_t raw_word_count
) {
    i2c_monitor_channel_state_t *channel_state = i2c_monitor_get_channel_state(channel);
    uint8_t active_buffer;

    if ((channel_state == NULL)
            || (channel_state->dma_channel < 0)
            || (raw_words == NULL)
            || (raw_word_count > I2C_MONITOR_BUFFER_WORDS)) {
        return false;
    }

    active_buffer = channel_state->active_buffer;
    memset(channel_state->buffers[active_buffer], 0, sizeof(channel_state->buffers[active_buffer]));
    memcpy(
        channel_state->buffers[active_buffer],
        raw_words,
        raw_word_count * sizeof(raw_words[0])
    );

    dma_hw->ints0 |= 1u << (uint32_t)channel_state->dma_channel;
    i2c_monitor_dma_irq_handler();
    return true;
}