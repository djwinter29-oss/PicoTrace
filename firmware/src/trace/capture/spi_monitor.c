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

/** @brief PIO instance reserved for the SPI monitor scaffold. */
static const PIO spi_monitor_pio = pio1;
/** @brief Session state for every logical SPI channel. */
static spi_monitor_channel_state_t g_spi_monitor_channels[SPI_MONITOR_CHANNEL_COUNT];
/** @brief Installed program offsets for SPI modes `0` through `3`. */
static uint32_t g_spi_monitor_program_offsets[4];
/** @brief Indicates whether shared SPI monitor setup completed successfully. */
static bool g_spi_monitor_initialized;
/** @brief Sticky failure flag preventing repeated partial initialization attempts. */
static bool g_spi_monitor_init_failed;

/** @brief Return whether a logical channel index is within the SPI monitor range. */
static bool spi_monitor_valid_channel(uint32_t channel) {
    return channel < SPI_MONITOR_CHANNEL_COUNT;
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

/** @copydoc spi_monitor_init */
spi_monitor_rc_t spi_monitor_init(void) {
    if (g_spi_monitor_initialized) {
        return SPI_MONITOR_RC_OK;
    }

    if (g_spi_monitor_init_failed) {
        return SPI_MONITOR_RC_FAILED;
    }

    memset(g_spi_monitor_channels, 0, sizeof(g_spi_monitor_channels));
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

/** @copydoc spi_monitor_set_channel_config */
spi_monitor_rc_t spi_monitor_set_channel_config(uint32_t channel, const spi_monitor_channel_config_t *config) {
    spi_monitor_channel_state_t *channel_state;
    uint32_t timeout_us;

    if (!g_spi_monitor_initialized || g_spi_monitor_init_failed) {
        return SPI_MONITOR_RC_FAILED;
    }

    if (!spi_monitor_valid_channel(channel) || (config == NULL)) {
        return SPI_MONITOR_RC_INVALID;
    }

    if (!spi_monitor_valid_capture(config->capture) || !spi_monitor_valid_mode(config->spi_mode)) {
        return SPI_MONITOR_RC_INVALID;
    }

    channel_state = &g_spi_monitor_channels[channel];
    if (config->capture == SPI_MONITOR_CAPTURE_DISABLED) {
        spi_monitor_reset_channel_state(channel_state);
        return SPI_MONITOR_RC_OK;
    }

    timeout_us = (config->timeout_us != 0u) ? config->timeout_us : SPI_MONITOR_TIMEOUT_US_DEFAULT;
    channel_state->running = true;
    channel_state->capture = config->capture;
    channel_state->spi_mode = config->spi_mode;
    channel_state->timeout_us = timeout_us;
    channel_state->packets_emitted = 0u;
    channel_state->overrun_count = 0u;
    return SPI_MONITOR_RC_OK;
}

/** @copydoc spi_monitor_get_channel_status */
spi_monitor_rc_t spi_monitor_get_channel_status(uint32_t channel, spi_monitor_channel_status_t *status_out) {
    if (!spi_monitor_valid_channel(channel) || (status_out == NULL)) {
        return SPI_MONITOR_RC_INVALID;
    }

    spi_monitor_fill_status(channel, status_out);
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