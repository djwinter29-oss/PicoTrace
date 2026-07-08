/**
 * @file spi_monitor.c
 * @brief SPI capture scaffold with per-channel start and stop configuration.
 */

#include "trace/capture/spi_monitor.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "hardware/pio.h"

#include "config/spi_monitor_config.h"

#include "spi_monitor.pio.h"

/** @brief Runtime state owned by one logical SPI channel in the current scaffold. */
typedef struct {
    bool running; /**< Indicates whether this logical channel is currently enabled. */
    spi_monitor_capture_t capture; /**< Active lane selection for this logical channel. */
    uint8_t spi_mode; /**< Active SPI mode `0` through `3`. */
    uint32_t timeout_us; /**< Active inter-byte timeout in microseconds. */
    uint32_t packets_emitted; /**< Number of emitted trace packet fragments in the current session. */
    uint32_t overrun_count; /**< Number of dropped completed fragments in the current session. */
} spi_monitor_channel_state_t;

/** @brief Runtime state shared by all logical channels that sit on one observed SPI bus. */
typedef struct {
    bool running; /**< Indicates whether this observed SPI bus is currently enabled. */
    spi_monitor_capture_t capture; /**< Bus-wide lane selection applied to all sibling logical channels. */
    uint8_t spi_mode; /**< Bus-wide SPI mode applied to all sibling logical channels. */
    uint8_t channel_select_mask; /**< Bit mask of selected chip-select slots on this bus. */
    uint32_t timeout_us; /**< Bus-wide inter-byte timeout applied to all sibling logical channels. */
} spi_monitor_bus_state_t;

/** @brief PIO instance reserved for the SPI monitor scaffold. */
static const PIO spi_monitor_pio = pio1;
/** @brief Session state for every logical SPI channel. */
static spi_monitor_channel_state_t g_spi_monitor_channels[SPI_MONITOR_CHANNEL_COUNT];
/** @brief Shared runtime state for each observed SPI bus. */
static spi_monitor_bus_state_t g_spi_monitor_buses[SPI_MONITOR_BUS_COUNT];
/** @brief Installed program offsets for SPI modes `0` through `3`. */
static uint32_t g_spi_monitor_program_offsets[4];
/** @brief Indicates whether shared SPI monitor setup completed successfully. */
static bool g_spi_monitor_initialized;
/** @brief Sticky failure flag preventing repeated partial initialization attempts. */
static bool g_spi_monitor_init_failed;

/** @brief Return whether an observed SPI bus index is within the SPI monitor range. */
static bool spi_monitor_valid_bus(uint32_t bus) {
    return bus < SPI_MONITOR_BUS_COUNT;
}

/** @brief Return whether the requested SPI mode is one of the four standard modes. */
static bool spi_monitor_valid_mode(uint8_t spi_mode) {
    return spi_mode <= 3u;
}

/** @brief Return whether the requested capture selection is supported by the scaffold. */
static bool spi_monitor_valid_capture(spi_monitor_capture_t capture) {
    return (capture == SPI_MONITOR_CAPTURE_DISABLED) ||
           (capture == SPI_MONITOR_CAPTURE_MOSI) ||
           (capture == SPI_MONITOR_CAPTURE_MOSI_MISO);
}

/** @brief Return whether the requested chip-select selection mask maps to at least one valid slot. */
static bool spi_monitor_valid_channel_select_mask(uint8_t channel_select_mask) {
    uint8_t valid_mask = SPI_MONITOR_CHANNEL_SELECT_ALL;

    return (channel_select_mask != 0u) && ((channel_select_mask & (uint8_t)~valid_mask) == 0u);
}

/** @brief Map one logical SPI channel index to its owning observed SPI bus index. */
static uint32_t spi_monitor_channel_to_bus(uint32_t channel) {
    return channel / SPI_MONITOR_CS_SLOTS_PER_BUS;
}

/** @brief Return the first logical SPI channel index owned by one observed SPI bus. */
static uint32_t spi_monitor_bus_first_channel(uint32_t bus) {
    return bus * SPI_MONITOR_CS_SLOTS_PER_BUS;
}

/** @brief Clear the shared bus state back to the stopped session state. */
static void spi_monitor_reset_bus_state(spi_monitor_bus_state_t *bus_state) {
    bus_state->running = false;
    bus_state->capture = SPI_MONITOR_CAPTURE_DISABLED;
    bus_state->spi_mode = 0u;
    bus_state->channel_select_mask = 0u;
    bus_state->timeout_us = 0u;
}

/** @brief Clear one logical SPI channel back to the stopped session state. */
static void spi_monitor_reset_channel_state(spi_monitor_channel_state_t *channel_state) {
    channel_state->running = false;
    channel_state->capture = SPI_MONITOR_CAPTURE_DISABLED;
    channel_state->spi_mode = 0u;
    channel_state->timeout_us = 0u;
    channel_state->packets_emitted = 0u;
    channel_state->overrun_count = 0u;
}

/** @brief Materialize one logical channel state into the public status snapshot. */
static void spi_monitor_fill_status(uint32_t channel, spi_monitor_channel_status_t *status_out) {
    const spi_monitor_channel_state_t *channel_state = &g_spi_monitor_channels[channel];

    memset(status_out, 0, sizeof(*status_out));
    status_out->initialized = g_spi_monitor_initialized && !g_spi_monitor_init_failed;
    status_out->running = channel_state->running;
    status_out->capture = channel_state->capture;
    status_out->spi_mode = channel_state->spi_mode;
    status_out->timeout_us = channel_state->timeout_us;
    status_out->packets_emitted = channel_state->packets_emitted;
    status_out->overrun_count = channel_state->overrun_count;
}

/** @brief Materialize one observed SPI bus state into the public bus status snapshot. */
static void spi_monitor_fill_bus_status(uint32_t bus, spi_monitor_bus_status_t *status_out) {
    const spi_monitor_bus_state_t *bus_state = &g_spi_monitor_buses[bus];
    const spi_monitor_channel_state_t *channel_state = &g_spi_monitor_channels[spi_monitor_bus_first_channel(bus)];

    memset(status_out, 0, sizeof(*status_out));
    status_out->initialized = g_spi_monitor_initialized && !g_spi_monitor_init_failed;
    status_out->running = bus_state->running;
    status_out->capture = bus_state->capture;
    status_out->spi_mode = bus_state->spi_mode;
    status_out->channel_select_mask = bus_state->channel_select_mask;
    status_out->timeout_us = bus_state->timeout_us;
    status_out->packets_emitted = channel_state->packets_emitted;
    status_out->overrun_count = channel_state->overrun_count;
}

/** @copydoc spi_monitor_init */
spi_monitor_rc_t spi_monitor_init(void) {
    if (g_spi_monitor_initialized) {
        return SPI_MONITOR_RC_OK;
    }

    if (g_spi_monitor_init_failed) {
        return SPI_MONITOR_RC_FAILED;
    }

    memset(g_spi_monitor_channels, 0, sizeof(g_spi_monitor_channels));
    memset(g_spi_monitor_buses, 0, sizeof(g_spi_monitor_buses));
    g_spi_monitor_program_offsets[0] = pio_add_program(spi_monitor_pio, &spi_monitor_mode0_sampler_program);
    g_spi_monitor_program_offsets[1] = pio_add_program(spi_monitor_pio, &spi_monitor_mode1_sampler_program);
    g_spi_monitor_program_offsets[2] = pio_add_program(spi_monitor_pio, &spi_monitor_mode2_sampler_program);
    g_spi_monitor_program_offsets[3] = pio_add_program(spi_monitor_pio, &spi_monitor_mode3_sampler_program);
    g_spi_monitor_initialized = true;
    return SPI_MONITOR_RC_OK;
}

/** @copydoc spi_monitor_poll */
void spi_monitor_poll(void) {
}

/** @copydoc spi_monitor_set_bus_config */
spi_monitor_rc_t spi_monitor_set_bus_config(uint32_t bus, const spi_monitor_bus_config_t *config) {
    spi_monitor_bus_state_t *bus_state;
    uint32_t channel;
    uint32_t first_channel;
    uint32_t timeout_us;

    if (!g_spi_monitor_initialized || g_spi_monitor_init_failed) {
        return SPI_MONITOR_RC_FAILED;
    }

    if (!spi_monitor_valid_bus(bus) || (config == NULL)) {
        return SPI_MONITOR_RC_INVALID;
    }

    if (!spi_monitor_valid_capture(config->capture) || !spi_monitor_valid_mode(config->spi_mode)) {
        return SPI_MONITOR_RC_INVALID;
    }

    bus_state = &g_spi_monitor_buses[bus];
    first_channel = spi_monitor_bus_first_channel(bus);
    if (config->capture == SPI_MONITOR_CAPTURE_DISABLED) {
        for (channel = first_channel; channel < (first_channel + SPI_MONITOR_CS_SLOTS_PER_BUS); ++channel) {
            spi_monitor_reset_channel_state(&g_spi_monitor_channels[channel]);
        }
        spi_monitor_reset_bus_state(bus_state);
        return SPI_MONITOR_RC_OK;
    }

    if (!spi_monitor_valid_channel_select_mask(config->channel_select_mask)) {
        return SPI_MONITOR_RC_INVALID;
    }

    timeout_us = (config->timeout_us != 0u) ? config->timeout_us : SPI_MONITOR_TIMEOUT_US_DEFAULT;
    bus_state->running = true;
    bus_state->capture = config->capture;
    bus_state->spi_mode = config->spi_mode;
    bus_state->channel_select_mask = config->channel_select_mask;
    bus_state->timeout_us = timeout_us;
    for (channel = first_channel; channel < (first_channel + SPI_MONITOR_CS_SLOTS_PER_BUS); ++channel) {
        spi_monitor_channel_state_t *channel_state = &g_spi_monitor_channels[channel];
        uint8_t channel_mask = (uint8_t)(1u << (channel - first_channel));

        channel_state->running = ((config->channel_select_mask & channel_mask) != 0u);
        channel_state->capture = channel_state->running ? config->capture : SPI_MONITOR_CAPTURE_DISABLED;
        channel_state->spi_mode = channel_state->running ? config->spi_mode : 0u;
        channel_state->timeout_us = channel_state->running ? timeout_us : 0u;
        channel_state->packets_emitted = 0u;
        channel_state->overrun_count = 0u;
    }
    return SPI_MONITOR_RC_OK;
}

/** @copydoc spi_monitor_get_bus_status */
spi_monitor_rc_t spi_monitor_get_bus_status(uint32_t bus, spi_monitor_bus_status_t *status_out) {
    if (!spi_monitor_valid_bus(bus) || (status_out == NULL)) {
        return SPI_MONITOR_RC_INVALID;
    }

    spi_monitor_fill_bus_status(bus, status_out);
    return SPI_MONITOR_RC_OK;
}

/** @copydoc spi_monitor_get_all_status */
spi_monitor_rc_t spi_monitor_get_all_status(spi_monitor_channel_status_t *status_out) {
    uint32_t channel;

    if (status_out == NULL) {
        return SPI_MONITOR_RC_INVALID;
    }

    for (channel = 0u; channel < SPI_MONITOR_CHANNEL_COUNT; ++channel) {
        spi_monitor_fill_status(channel, &status_out[channel]);
    }

    return SPI_MONITOR_RC_OK;
}