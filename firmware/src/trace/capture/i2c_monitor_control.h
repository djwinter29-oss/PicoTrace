/**
 * @file i2c_monitor_control.h
 * @brief Control bridge for routing I2C monitor commands onto the producer core.
 */

#ifndef I2C_MONITOR_CONTROL_H
#define I2C_MONITOR_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

#include "trace/capture/i2c_monitor.h"

typedef bool (*i2c_monitor_control_set_channel_fn)(uint32_t channel, uint32_t sample_hz);
typedef bool (*i2c_monitor_control_get_status_fn)(uint32_t channel, i2c_monitor_channel_status_t *status_out);

void i2c_monitor_control_init(void);
void i2c_monitor_control_bind_executor(
    i2c_monitor_control_set_channel_fn set_channel_fn,
    i2c_monitor_control_get_status_fn get_status_fn
);
void i2c_monitor_control_set_inline_mode(bool enabled);
bool i2c_monitor_control_set_channel_sample_hz(uint32_t channel, uint32_t sample_hz);
bool i2c_monitor_control_get_channel_status(uint32_t channel, i2c_monitor_channel_status_t *status_out);
bool i2c_monitor_control_poll(void);

#endif