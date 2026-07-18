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

/** @brief Capture direction selection requested for one observed SPI bus. */
typedef enum {
    SPI_MONITOR_CAPTURE_DISABLED = 0u,
    SPI_MONITOR_CAPTURE_MOSI = 1u,
    SPI_MONITOR_CAPTURE_MOSI_MISO = 2u,
} spi_monitor_capture_t;

/** @brief Select all observed chip-select slots on one SPI bus. */
#define SPI_MONITOR_CHANNEL_SELECT_ALL ((uint8_t)((1u << SPI_MONITOR_CS_SLOTS_PER_BUS) - 1u))

/** @brief Requested runtime configuration for one observed SPI bus. */
typedef struct {
    spi_monitor_capture_t capture; /**< Disabled, MOSI-only, or MOSI+MISO capture. */
    uint8_t spi_mode; /**< SPI mode `0` through `3`. */
    uint8_t channel_select_mask; /**< Bit mask of selected `CS_N` slots on this bus, or @ref SPI_MONITOR_CHANNEL_SELECT_ALL. */
    uint32_t timeout_us; /**< Inter-byte timeout in microseconds, or `0` for the default. */
} spi_monitor_bus_config_t;

/** @brief Snapshot of one observed SPI bus control state. */
typedef struct {
    bool initialized; /**< Indicates whether the shared SPI monitor resources initialized successfully. */
    bool running; /**< Indicates whether capture is currently active for this observed SPI bus. */
    spi_monitor_capture_t capture; /**< Active capture direction for this observed SPI bus. */
    uint8_t spi_mode; /**< Active SPI mode `0` through `3`. */
    uint8_t channel_select_mask; /**< Bit mask of selected `CS_N` slots on this bus. */
    uint32_t timeout_us; /**< Active inter-byte timeout in microseconds. */
    uint32_t packets_emitted; /**< Number of emitted trace packet fragments in the current session. */
    uint32_t overrun_count; /**< Number of bus-visible dropped fragments in the current session, including sampler and sink loss. */
} spi_monitor_bus_status_t;

/** @brief Snapshot of one logical SPI monitor channel. */
typedef struct {
    bool initialized; /**< Indicates whether the shared SPI monitor resources initialized successfully. */
    bool running; /**< Indicates whether capture is currently active for this logical channel. */
    spi_monitor_capture_t capture; /**< Active capture direction for this logical channel. */
    uint8_t spi_mode; /**< Active SPI mode `0` through `3`. */
    uint32_t timeout_us; /**< Active inter-byte timeout in microseconds. */
    uint32_t packets_emitted; /**< Number of emitted trace packet fragments in the current session. */
    uint32_t overrun_count; /**< Number of channel-attributable trace packet fragments dropped in the current session. */
} spi_monitor_channel_status_t;

/**
 * @brief Initialize the shared SPI monitor scaffold.
 *
 * This installs the SPI PIO programs and leaves all observed SPI buses stopped until the producer
 * core applies a non-disabled bus configuration.
 *
 * @return Control result describing whether setup succeeded or failed.
 */
spi_monitor_rc_t spi_monitor_init(void);

/** @brief Return whether the producer core currently has SPI monitor work to service. */
bool spi_monitor_needs_poll(void);

/**
 * @brief Service SPI monitor background work on the producer core.
 *
 * This hook polls the active bus samplers, closes timed-out transactions, and emits completed SPI
 * trace packet fragments into the shared trace ring.
 */
void spi_monitor_poll(void);

/**
 * @brief Start, stop, or reconfigure one observed SPI bus.
 * @param bus Zero-based observed SPI bus index.
 * @param config Caller-owned capture configuration.
 * @return Control result describing whether the request was applied or rejected.
 *
 * Passing @ref SPI_MONITOR_CAPTURE_DISABLED stops the whole observed SPI bus and clears the session
 * counters of all logical channels that belong to it. Passing a new active configuration restarts
 * the bus with fresh session state for every logical channel on that bus.
 */
spi_monitor_rc_t spi_monitor_set_bus_config(uint32_t bus, const spi_monitor_bus_config_t *config);

/**
 * @brief Read the current control state of one observed SPI bus.
 * @param bus Zero-based observed SPI bus index.
 * @param status_out Caller-owned destination for the status snapshot.
 * @return Control result describing whether the snapshot was returned or the request was invalid.
 */
spi_monitor_rc_t spi_monitor_get_bus_status(uint32_t bus, spi_monitor_bus_status_t *status_out);

/**
 * @brief Read a coherent snapshot for every logical SPI channel in one producer-core-owned operation.
 * @param status_out Caller-owned array of @ref SPI_MONITOR_CHANNEL_COUNT status entries.
 * @return Control result describing whether the snapshot was returned or the request was invalid.
 */
spi_monitor_rc_t spi_monitor_get_all_status(spi_monitor_channel_status_t *status_out);

#endif