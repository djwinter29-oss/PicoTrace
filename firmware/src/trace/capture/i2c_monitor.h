/**
 * @file i2c_monitor.h
 * @brief Public interface for the per-channel I2C raw sampler.
 *
 * The sampler owns the PIO and DMA bring-up for passive I2C observation and forwards completed raw
 * sample buffers to the I2C decoder before decoded event fragments are pushed into the trace ring.
 */

#ifndef I2C_MONITOR_H
#define I2C_MONITOR_H

#include <stdbool.h>

/**
 * @brief Initialize the I2C sampler state machines, DMA channels, and ping-pong buffers.
 *
 * This function is idempotent. Repeated calls after successful initialization have no effect.
 * Repeated calls after a failed initialization return `false` without retrying.
 * @return `true` when every sampler channel was initialized, or `false` when setup failed.
 */
bool i2c_monitor_init(void);

#endif