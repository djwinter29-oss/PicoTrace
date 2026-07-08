/**
 * @file spi_monitor_control.c
 * @brief Blocking command bridge for producer-core-owned SPI monitor control.
 */

#include "trace/capture/spi_monitor_control.h"

#include <string.h>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

/** @brief Mailbox lifecycle for one pending SPI monitor control command. */
typedef enum {
    SPI_MONITOR_CONTROL_STATE_IDLE = 0u, /**< No command is currently staged. */
    SPI_MONITOR_CONTROL_STATE_PENDING = 1u, /**< A non-owner core staged a command for the producer core. */
    SPI_MONITOR_CONTROL_STATE_COMPLETE = 2u, /**< The producer core finished the staged command. */
} spi_monitor_control_state_t;

/** @brief Operation kind staged through the SPI monitor control mailbox. */
typedef enum {
    SPI_MONITOR_CONTROL_COMMAND_NONE = 0u, /**< No operation is staged. */
    SPI_MONITOR_CONTROL_COMMAND_SET_CONFIG = 1u, /**< Apply one logical channel configuration. */
    SPI_MONITOR_CONTROL_COMMAND_GET_STATUS = 2u, /**< Read one logical channel status snapshot. */
    SPI_MONITOR_CONTROL_COMMAND_GET_ALL_STATUS = 3u, /**< Read all logical channel status snapshots. */
} spi_monitor_control_command_t;

/** @brief Shared mailbox carrying one SPI monitor command and its reply fields. */
typedef struct {
    volatile uint32_t state; /**< Current mailbox lifecycle state. */
    uint32_t command; /**< Staged @ref spi_monitor_control_command_t value. */
    uint32_t channel; /**< Zero-based logical channel index for single-channel operations. */
    spi_monitor_channel_config_t config; /**< Caller-supplied configuration for set-config requests. */
    spi_monitor_rc_t result; /**< Result written by the producer-core executor. */
    spi_monitor_channel_status_t status[SPI_MONITOR_CHANNEL_COUNT]; /**< Reply buffer for single or all-channel status reads. */
} spi_monitor_control_mailbox_t;

/** @brief Shared mailbox instance used by non-owner cores and the producer core. */
static spi_monitor_control_mailbox_t g_spi_monitor_control_mailbox;
/** @brief Bound producer-core executor for configuration changes. */
static spi_monitor_control_set_channel_fn g_spi_monitor_control_set_channel_fn;
/** @brief Bound producer-core executor for single-channel status reads. */
static spi_monitor_control_get_status_fn g_spi_monitor_control_get_status_fn;
/** @brief Bound producer-core executor for all-channel status reads. */
static spi_monitor_control_get_all_status_fn g_spi_monitor_control_get_all_status_fn;
/** @brief Host-test shortcut that bypasses the mailbox path. */
static bool g_spi_monitor_control_inline_mode;

/** @brief Prevent the wait loops from collapsing away while polling the mailbox state. */
static void spi_monitor_control_wait_hint(void) {
#if defined(_MSC_VER)
    _ReadWriteBarrier();
#else
    __asm volatile("" ::: "memory");
#endif
}

/** @brief Acquire-load the mailbox state. */
static uint32_t spi_monitor_control_load_state(void) {
#if defined(_MSC_VER)
    _ReadWriteBarrier();
    return g_spi_monitor_control_mailbox.state;
#else
    return __atomic_load_n(&g_spi_monitor_control_mailbox.state, __ATOMIC_ACQUIRE);
#endif
}

/** @brief Release-store the mailbox state. */
static void spi_monitor_control_store_state(uint32_t state) {
#if defined(_MSC_VER)
    g_spi_monitor_control_mailbox.state = state;
    _ReadWriteBarrier();
#else
    __atomic_store_n(&g_spi_monitor_control_mailbox.state, state, __ATOMIC_RELEASE);
#endif
}

/** @copydoc spi_monitor_control_init */
void spi_monitor_control_init(void) {
    memset(&g_spi_monitor_control_mailbox, 0, sizeof(g_spi_monitor_control_mailbox));
    g_spi_monitor_control_set_channel_fn = NULL;
    g_spi_monitor_control_get_status_fn = NULL;
    g_spi_monitor_control_get_all_status_fn = NULL;
    g_spi_monitor_control_inline_mode = false;
}

/** @copydoc spi_monitor_control_bind_executor */
void spi_monitor_control_bind_executor(
    spi_monitor_control_set_channel_fn set_channel_fn,
    spi_monitor_control_get_status_fn get_status_fn,
    spi_monitor_control_get_all_status_fn get_all_status_fn
) {
    g_spi_monitor_control_set_channel_fn = set_channel_fn;
    g_spi_monitor_control_get_status_fn = get_status_fn;
    g_spi_monitor_control_get_all_status_fn = get_all_status_fn;
}

/** @copydoc spi_monitor_control_set_inline_mode */
void spi_monitor_control_set_inline_mode(bool enabled) {
    g_spi_monitor_control_inline_mode = enabled;
}

/** @copydoc spi_monitor_control_set_channel_config */
spi_monitor_rc_t spi_monitor_control_set_channel_config(uint32_t channel, const spi_monitor_channel_config_t *config) {
    if (g_spi_monitor_control_inline_mode) {
        if ((g_spi_monitor_control_set_channel_fn == NULL) || (config == NULL)) {
            return SPI_MONITOR_RC_INVALID;
        }

        return g_spi_monitor_control_set_channel_fn(channel, config);
    }

    if (config == NULL) {
        return SPI_MONITOR_RC_INVALID;
    }

    while (spi_monitor_control_load_state() != SPI_MONITOR_CONTROL_STATE_IDLE) {
        spi_monitor_control_wait_hint();
    }

    g_spi_monitor_control_mailbox.command = SPI_MONITOR_CONTROL_COMMAND_SET_CONFIG;
    g_spi_monitor_control_mailbox.channel = channel;
    g_spi_monitor_control_mailbox.config = *config;
    spi_monitor_control_store_state(SPI_MONITOR_CONTROL_STATE_PENDING);

    while (spi_monitor_control_load_state() != SPI_MONITOR_CONTROL_STATE_COMPLETE) {
        spi_monitor_control_wait_hint();
    }

    spi_monitor_control_store_state(SPI_MONITOR_CONTROL_STATE_IDLE);
    return g_spi_monitor_control_mailbox.result;
}

/** @copydoc spi_monitor_control_get_channel_status */
spi_monitor_rc_t spi_monitor_control_get_channel_status(uint32_t channel, spi_monitor_channel_status_t *status_out) {
    if (g_spi_monitor_control_inline_mode) {
        if ((g_spi_monitor_control_get_status_fn == NULL) || (status_out == NULL)) {
            return SPI_MONITOR_RC_INVALID;
        }

        return g_spi_monitor_control_get_status_fn(channel, status_out);
    }

    if (status_out == NULL) {
        return SPI_MONITOR_RC_INVALID;
    }

    while (spi_monitor_control_load_state() != SPI_MONITOR_CONTROL_STATE_IDLE) {
        spi_monitor_control_wait_hint();
    }

    g_spi_monitor_control_mailbox.command = SPI_MONITOR_CONTROL_COMMAND_GET_STATUS;
    g_spi_monitor_control_mailbox.channel = channel;
    spi_monitor_control_store_state(SPI_MONITOR_CONTROL_STATE_PENDING);

    while (spi_monitor_control_load_state() != SPI_MONITOR_CONTROL_STATE_COMPLETE) {
        spi_monitor_control_wait_hint();
    }

    *status_out = g_spi_monitor_control_mailbox.status[0];
    spi_monitor_control_store_state(SPI_MONITOR_CONTROL_STATE_IDLE);
    return g_spi_monitor_control_mailbox.result;
}

/** @copydoc spi_monitor_control_get_all_status */
spi_monitor_rc_t spi_monitor_control_get_all_status(spi_monitor_channel_status_t *status_out) {
    if (g_spi_monitor_control_inline_mode) {
        if ((g_spi_monitor_control_get_all_status_fn == NULL) || (status_out == NULL)) {
            return SPI_MONITOR_RC_INVALID;
        }

        return g_spi_monitor_control_get_all_status_fn(status_out);
    }

    if (status_out == NULL) {
        return SPI_MONITOR_RC_INVALID;
    }

    while (spi_monitor_control_load_state() != SPI_MONITOR_CONTROL_STATE_IDLE) {
        spi_monitor_control_wait_hint();
    }

    g_spi_monitor_control_mailbox.command = SPI_MONITOR_CONTROL_COMMAND_GET_ALL_STATUS;
    spi_monitor_control_store_state(SPI_MONITOR_CONTROL_STATE_PENDING);

    while (spi_monitor_control_load_state() != SPI_MONITOR_CONTROL_STATE_COMPLETE) {
        spi_monitor_control_wait_hint();
    }

    memcpy(
        status_out,
        g_spi_monitor_control_mailbox.status,
        SPI_MONITOR_CHANNEL_COUNT * sizeof(g_spi_monitor_control_mailbox.status[0])
    );
    spi_monitor_control_store_state(SPI_MONITOR_CONTROL_STATE_IDLE);
    return g_spi_monitor_control_mailbox.result;
}

/** @copydoc spi_monitor_control_poll */
bool spi_monitor_control_poll(void) {
    if (spi_monitor_control_load_state() != SPI_MONITOR_CONTROL_STATE_PENDING) {
        return false;
    }

    switch ((spi_monitor_control_command_t)g_spi_monitor_control_mailbox.command) {
        case SPI_MONITOR_CONTROL_COMMAND_SET_CONFIG:
            g_spi_monitor_control_mailbox.result = (g_spi_monitor_control_set_channel_fn == NULL)
                ? SPI_MONITOR_RC_FAILED
                : g_spi_monitor_control_set_channel_fn(
                    g_spi_monitor_control_mailbox.channel,
                    &g_spi_monitor_control_mailbox.config
                );
            break;

        case SPI_MONITOR_CONTROL_COMMAND_GET_STATUS:
            g_spi_monitor_control_mailbox.result = (g_spi_monitor_control_get_status_fn == NULL)
                ? SPI_MONITOR_RC_INVALID
                : g_spi_monitor_control_get_status_fn(
                    g_spi_monitor_control_mailbox.channel,
                    &g_spi_monitor_control_mailbox.status[0]
                );
            break;

        case SPI_MONITOR_CONTROL_COMMAND_GET_ALL_STATUS:
            g_spi_monitor_control_mailbox.result = (g_spi_monitor_control_get_all_status_fn == NULL)
                ? SPI_MONITOR_RC_INVALID
                : g_spi_monitor_control_get_all_status_fn(g_spi_monitor_control_mailbox.status);
            break;

        case SPI_MONITOR_CONTROL_COMMAND_NONE:
        default:
            g_spi_monitor_control_mailbox.result = SPI_MONITOR_RC_INVALID;
            break;
    }

    spi_monitor_control_store_state(SPI_MONITOR_CONTROL_STATE_COMPLETE);
    return true;
}