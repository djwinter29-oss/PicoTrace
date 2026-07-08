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

#include "config/i2c_monitor_config.h"

/** @brief Result returned by producer-core-owned I2C monitor control operations. */
typedef enum {
	I2C_MONITOR_RC_OK = 0u,
	I2C_MONITOR_RC_BUSY = 1u,
	I2C_MONITOR_RC_DISABLED = 2u,
	I2C_MONITOR_RC_INVALID = 3u,
	I2C_MONITOR_RC_FAILED = 4u,
} i2c_monitor_rc_t;

/** @brief Snapshot of one monitor channel state. */
typedef struct {
	bool initialized; /**< Indicates whether the monitor shared resources were initialized. */
	bool running; /**< Indicates whether the channel is currently sampling. */
	bool overrun; /**< Sticky flag set when one or more decoded fragments could not be queued. */
	bool transition_pending; /**< Indicates whether the producer core still needs to complete a latched transition. */
	uint8_t transition_reason; /**< Pending boundary event type when @ref transition_pending is set, otherwise `0`. */
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
 * Repeated calls after a failed initialization return @ref I2C_MONITOR_RC_FAILED without retrying.
 * @return Control result describing whether setup succeeded or failed.
 */
i2c_monitor_rc_t i2c_monitor_init(void);

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
 * @return Control result describing whether the request was applied, blocked by a pending
 * transition, rejected because streaming is disabled, or invalid/failed.
 *
 * Passing a different non-zero sample rate is also destructive: the current channel instance is
 * stopped, its runtime decode and packet state is discarded, and the channel is started again with
 * the new rate.
 *
 * This API is owned by the producer core that initialized the monitor. Callers on other cores must
 * route requests onto that core instead of touching the monitor directly.
 */
i2c_monitor_rc_t i2c_monitor_set_channel_sample_hz(uint32_t channel, uint32_t sample_hz);

/**
 * @brief Read the current state of one monitor channel.
 * @param channel Zero-based monitor channel index.
 * @param status_out Caller-owned destination for the status snapshot.
 * @return Control result describing whether the snapshot was returned or the request was invalid.
 *
 * Status reads remain available while the producer core still owns a pending transition. In that
 * case @ref i2c_monitor_channel_status_t.transition_pending is set and
 * @ref i2c_monitor_channel_status_t.transition_reason reports the latched boundary event type.
 */
i2c_monitor_rc_t i2c_monitor_get_channel_status(uint32_t channel, i2c_monitor_channel_status_t *status_out);

/**
 * @brief Read a coherent snapshot for every monitor channel in one producer-core-owned operation.
 * @param status_out Caller-owned array of @ref I2C_MONITOR_CHANNEL_COUNT status entries.
 * @return Control result describing whether the snapshot was returned or the request was invalid.
 */
i2c_monitor_rc_t i2c_monitor_get_all_status(i2c_monitor_channel_status_t *status_out);

#endif