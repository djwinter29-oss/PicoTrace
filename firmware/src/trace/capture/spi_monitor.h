/**
 * @file spi_monitor.h
 * @brief Public interface for the SPI capture scaffold.
 */

#ifndef SPI_MONITOR_H
#define SPI_MONITOR_H

#include <stdbool.h>
#include <stdint.h>

#include "config/spi_monitor_config.h"

/** @brief Result returned by producer-core-owned SPI monitor control operations. */
typedef enum {
    SPI_MONITOR_RC_OK = 0u,
    SPI_MONITOR_RC_BUSY = 1u,
    SPI_MONITOR_RC_DISABLED = 2u,
    SPI_MONITOR_RC_INVALID = 3u,
    SPI_MONITOR_RC_FAILED = 4u,
} spi_monitor_rc_t;

/** @brief Capture lane selection requested for one logical SPI channel. */
typedef enum {
    SPI_MONITOR_CAPTURE_DISABLED = 0u,
    SPI_MONITOR_CAPTURE_MOSI = 1u,
    SPI_MONITOR_CAPTURE_MOSI_MISO = 2u,
} spi_monitor_capture_t;

/** @brief Requested runtime configuration for one logical SPI channel. */
typedef struct {
    spi_monitor_capture_t capture; /**< Disabled, MOSI-only, or MOSI+MISO capture. */
    uint8_t spi_mode; /**< SPI mode `0` through `3`. */
    uint32_t timeout_us; /**< Inter-byte timeout in microseconds, or `0` for the default. */
} spi_monitor_channel_config_t;

/** @brief Snapshot of one logical SPI monitor channel. */
typedef struct {
    bool initialized; /**< Indicates whether the shared SPI monitor resources initialized successfully. */
    bool running; /**< Indicates whether capture is currently active for this logical channel. */
    spi_monitor_capture_t capture; /**< Active lane selection for this logical channel. */
    uint8_t spi_mode; /**< Active SPI mode `0` through `3`. */
    uint32_t timeout_us; /**< Active inter-byte timeout in microseconds. */
    uint32_t packets_emitted; /**< Number of emitted trace packet fragments in the current session. */
    uint32_t overrun_count; /**< Number of dropped completed fragments in the current session. */
} spi_monitor_channel_status_t;

/**
 * @brief Initialize the shared SPI monitor scaffold.
 *
 * This installs the SPI PIO programs and leaves all logical channels stopped until the producer
 * core applies a non-disabled channel configuration.
 *
 * @return Control result describing whether setup succeeded or failed.
 */
spi_monitor_rc_t spi_monitor_init(void);

/**
 * @brief Service SPI monitor background work on the producer core.
 *
 * The current scaffold has no deferred runtime work yet, but the final implementation will use
 * this hook for bus polling, timeout closure, and packet emission.
 */
void spi_monitor_poll(void);

/**
 * @brief Start, stop, or reconfigure one logical SPI channel.
 * @param channel Zero-based SPI logical channel index.
 * @param config Caller-owned capture configuration.
 * @return Control result describing whether the request was applied or rejected.
 *
 * Passing @ref SPI_MONITOR_CAPTURE_DISABLED stops the logical channel and clears its session
 * counters. Passing a new active configuration restarts the logical channel with fresh session
 * state.
 */
spi_monitor_rc_t spi_monitor_set_channel_config(uint32_t channel, const spi_monitor_channel_config_t *config);

/**
 * @brief Read the current state of one logical SPI channel.
 * @param channel Zero-based SPI logical channel index.
 * @param status_out Caller-owned destination for the status snapshot.
 * @return Control result describing whether the snapshot was returned or the request was invalid.
 */
spi_monitor_rc_t spi_monitor_get_channel_status(uint32_t channel, spi_monitor_channel_status_t *status_out);

/**
 * @brief Read a coherent snapshot for every logical SPI channel in one producer-core-owned operation.
 * @param status_out Caller-owned array of @ref SPI_MONITOR_CHANNEL_COUNT status entries.
 * @return Control result describing whether the snapshot was returned or the request was invalid.
 */
spi_monitor_rc_t spi_monitor_get_all_status(spi_monitor_channel_status_t *status_out);

#endif