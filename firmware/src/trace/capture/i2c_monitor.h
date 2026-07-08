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
#include <stdint.h>

/** @brief Snapshot of one monitor channel state. */
typedef struct {
	bool initialized; /**< Indicates whether the monitor shared resources were initialized. */
	bool running; /**< Indicates whether the channel is currently sampling. */
	bool overrun; /**< Sticky flag set when one or more decoded fragments could not be queued. */
	uint32_t sample_hz; /**< Active oversampling rate for the channel, or `0` when stopped. */
	uint32_t completed_buffers; /**< Number of ping-pong buffers completed since the current start. */
	uint32_t overrun_count; /**< Number of times decoded output could not be queued since the current start. */
} i2c_monitor_channel_status_t;

/**
 * @brief Initialize the shared I2C sampler resources.
 *
 * This function installs the PIO program, prepares the DMA IRQ handler, and leaves every channel
 * stopped until @ref i2c_monitor_set_channel_sample_hz is called with a non-zero sample rate.
 * It is idempotent. Repeated calls after successful initialization have no effect.
 * Repeated calls after a failed initialization return `false` without retrying.
 * @return `true` when every sampler channel was initialized, or `false` when setup failed.
 */
bool i2c_monitor_init(void);

/**
 * @brief Service completed raw sample buffers on the producer core.
 *
 * This function drains any raw sample blocks staged by the DMA IRQ handler, stops active monitor
 * channels when streaming is disabled, and runs decode plus packetization outside the hard IRQ
 * path.
 * Call it regularly from the same producer core that owns the monitor.
 */
void i2c_monitor_poll(void);

/**
 * @brief Start or stop one monitor channel.
 * @param channel Zero-based monitor channel index.
 * @param sample_hz Oversampling rate in Hz, or `0` to stop sampling and reset the channel state.
 * @return `true` when the request was applied, or `false` when the input was invalid or setup failed.
 *
 * Passing a different non-zero sample rate is also destructive: the current channel instance is
 * stopped, its runtime decode and packet state is discarded, and the channel is started again with
 * the new rate.
 *
 * This API is owned by the producer core that initialized the monitor. Callers on other cores must
 * route requests onto that core instead of touching the monitor directly.
 */
bool i2c_monitor_set_channel_sample_hz(uint32_t channel, uint32_t sample_hz);

/**
 * @brief Read the current state of one monitor channel.
 * @param channel Zero-based monitor channel index.
 * @param status_out Caller-owned destination for the status snapshot.
 * @return `true` when the channel index and output pointer were valid, otherwise `false`.
 */
bool i2c_monitor_get_channel_status(uint32_t channel, i2c_monitor_channel_status_t *status_out);

#endif