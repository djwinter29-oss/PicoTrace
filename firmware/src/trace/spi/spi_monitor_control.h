/**
 * @file spi_monitor_control.h
 * @brief Control bridge for routing SPI monitor commands onto the producer core.
 */

#ifndef SPI_MONITOR_CONTROL_H
#define SPI_MONITOR_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

#include "trace/spi/spi_monitor.h"

/** @brief Producer-core executor signature for SPI monitor bus configuration changes. */
typedef spi_monitor_rc_t (*spi_monitor_control_set_bus_fn)(
    uint32_t bus,
    const spi_monitor_bus_config_t *config
);
/** @brief Producer-core executor signature for one SPI monitor bus status read. */
typedef spi_monitor_rc_t (*spi_monitor_control_get_bus_status_fn)(
    uint32_t bus,
    spi_monitor_bus_status_t *status_out
);
/** @brief Producer-core executor signature for an all-channel SPI monitor status read. */
typedef spi_monitor_rc_t (*spi_monitor_control_get_all_status_fn)(spi_monitor_channel_status_t *status_out);

/** @brief Reset the SPI monitor control mailbox and executor bindings. */
void spi_monitor_control_init(void);

/**
 * @brief Bind producer-core SPI monitor executors to the control bridge.
 * @param set_bus_fn Executor used for configuration changes.
 * @param get_bus_status_fn Executor used for single-bus status reads.
 * @param get_all_status_fn Executor used for all-channel status reads.
 */
void spi_monitor_control_bind_executor(
    spi_monitor_control_set_bus_fn set_bus_fn,
    spi_monitor_control_get_bus_status_fn get_bus_status_fn,
    spi_monitor_control_get_all_status_fn get_all_status_fn
);

/**
 * @brief Select direct inline execution for host tests.
 * @param enabled When true, bypass the mailbox and call bound executors directly.
 */
void spi_monitor_control_set_inline_mode(bool enabled);

/**
 * @brief Queue an observed SPI bus configuration change onto the producer core.
 * @param bus Zero-based observed SPI bus index.
 * @param config Caller-owned bus configuration.
 * @return Control result describing whether the request was applied or rejected.
 */
spi_monitor_rc_t spi_monitor_control_set_bus_config(uint32_t bus, const spi_monitor_bus_config_t *config);

/**
 * @brief Queue a single-bus SPI monitor status read onto the producer core.
 * @param bus Zero-based observed SPI bus index.
 * @param status_out Caller-owned destination for the status snapshot.
 * @return Control result describing whether the snapshot was returned or the request was invalid.
 */
spi_monitor_rc_t spi_monitor_control_get_bus_status(uint32_t bus, spi_monitor_bus_status_t *status_out);

/**
 * @brief Queue an all-channel SPI monitor status read onto the producer core.
 * @param status_out Caller-owned array of @ref SPI_MONITOR_CHANNEL_COUNT status entries.
 * @return Control result describing whether the snapshot was returned or the request was invalid.
 */
spi_monitor_rc_t spi_monitor_control_get_all_status(spi_monitor_channel_status_t *status_out);

/**
 * @brief Service one pending SPI monitor mailbox command on the producer core.
 * @return `true` when a pending command was executed, otherwise `false`.
 */
bool spi_monitor_control_poll(void);

#endif