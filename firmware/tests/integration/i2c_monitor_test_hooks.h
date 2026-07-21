#ifndef I2C_MONITOR_TEST_HOOKS_H
#define I2C_MONITOR_TEST_HOOKS_H

#include <stdbool.h>
#include <stdint.h>

void i2c_monitor_test_reset(void);
bool i2c_monitor_test_start_channel(uint32_t channel, uint32_t sample_hz);
bool i2c_monitor_test_feed_completed_buffer(
    uint32_t channel,
    const uint32_t *raw_words,
    uint32_t raw_word_count
);

#endif