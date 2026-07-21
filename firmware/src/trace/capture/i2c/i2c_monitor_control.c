/**
 * @file i2c_monitor_control.c
 * @brief Blocking command bridge for producer-core-owned I2C monitor control.
 */

#include "trace/capture/i2c/i2c_monitor_control.h"

#include <string.h>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

typedef enum {
    I2C_MONITOR_CONTROL_STATE_IDLE = 0u,
    I2C_MONITOR_CONTROL_STATE_PENDING = 1u,
    I2C_MONITOR_CONTROL_STATE_COMPLETE = 2u,
} i2c_monitor_control_state_t;

typedef enum {
    I2C_MONITOR_CONTROL_COMMAND_NONE = 0u,
    I2C_MONITOR_CONTROL_COMMAND_SET_RATE = 1u,
    I2C_MONITOR_CONTROL_COMMAND_GET_STATUS = 2u,
    I2C_MONITOR_CONTROL_COMMAND_GET_ALL_STATUS = 3u,
} i2c_monitor_control_command_t;

typedef struct {
    volatile uint32_t state;
    uint32_t command;
    uint32_t channel;
    uint32_t sample_hz;
    i2c_monitor_rc_t result;
    i2c_monitor_channel_status_t status[I2C_MONITOR_CHANNEL_COUNT];
} i2c_monitor_control_mailbox_t;

static i2c_monitor_control_mailbox_t g_i2c_monitor_control_mailbox;
static i2c_monitor_control_set_channel_fn g_i2c_monitor_control_set_channel_fn;
static i2c_monitor_control_get_status_fn g_i2c_monitor_control_get_status_fn;
static i2c_monitor_control_get_all_status_fn g_i2c_monitor_control_get_all_status_fn;
static bool g_i2c_monitor_control_inline_mode;

/** @brief Maximum spin iterations while waiting on mailbox state transitions. */
#define I2C_MONITOR_CONTROL_WAIT_SPINS 1000000u

static void i2c_monitor_control_wait_hint(void) {
#if defined(_MSC_VER)
    _ReadWriteBarrier();
#else
    __asm volatile("" ::: "memory");
#endif
}

static uint32_t i2c_monitor_control_load_state(void) {
#if defined(_MSC_VER)
    _ReadWriteBarrier();
    return g_i2c_monitor_control_mailbox.state;
#else
    return __atomic_load_n(&g_i2c_monitor_control_mailbox.state, __ATOMIC_ACQUIRE);
#endif
}

static void i2c_monitor_control_store_state(uint32_t state) {
#if defined(_MSC_VER)
    g_i2c_monitor_control_mailbox.state = state;
    _ReadWriteBarrier();
#else
    __atomic_store_n(&g_i2c_monitor_control_mailbox.state, state, __ATOMIC_RELEASE);
#endif
}

static bool i2c_monitor_control_wait_until_idle(void) {
    uint32_t wait_spins = I2C_MONITOR_CONTROL_WAIT_SPINS;

    while (true) {
        uint32_t state = i2c_monitor_control_load_state();

        if (state == I2C_MONITOR_CONTROL_STATE_IDLE) {
            return true;
        }

        if (state == I2C_MONITOR_CONTROL_STATE_COMPLETE) {
            i2c_monitor_control_store_state(I2C_MONITOR_CONTROL_STATE_IDLE);
            return true;
        }

        if (wait_spins == 0u) {
            return false;
        }

        wait_spins -= 1u;
        i2c_monitor_control_wait_hint();
    }
}

void i2c_monitor_control_init(void) {
    memset(&g_i2c_monitor_control_mailbox, 0, sizeof(g_i2c_monitor_control_mailbox));
    g_i2c_monitor_control_set_channel_fn = NULL;
    g_i2c_monitor_control_get_status_fn = NULL;
    g_i2c_monitor_control_get_all_status_fn = NULL;
    g_i2c_monitor_control_inline_mode = false;
}

void i2c_monitor_control_bind_executor(
    i2c_monitor_control_set_channel_fn set_channel_fn,
    i2c_monitor_control_get_status_fn get_status_fn,
    i2c_monitor_control_get_all_status_fn get_all_status_fn
) {
    g_i2c_monitor_control_set_channel_fn = set_channel_fn;
    g_i2c_monitor_control_get_status_fn = get_status_fn;
    g_i2c_monitor_control_get_all_status_fn = get_all_status_fn;
}

void i2c_monitor_control_set_inline_mode(bool enabled) {
    g_i2c_monitor_control_inline_mode = enabled;
}

i2c_monitor_rc_t i2c_monitor_control_set_channel_sample_hz(uint32_t channel, uint32_t sample_hz) {
    uint32_t wait_spins;

    if (g_i2c_monitor_control_inline_mode) {
        if (g_i2c_monitor_control_set_channel_fn == NULL) {
            return I2C_MONITOR_RC_FAILED;
        }

        return g_i2c_monitor_control_set_channel_fn(channel, sample_hz);
    }

    if (!i2c_monitor_control_wait_until_idle()) {
        return I2C_MONITOR_RC_BUSY;
    }

    g_i2c_monitor_control_mailbox.command = I2C_MONITOR_CONTROL_COMMAND_SET_RATE;
    g_i2c_monitor_control_mailbox.channel = channel;
    g_i2c_monitor_control_mailbox.sample_hz = sample_hz;
    i2c_monitor_control_store_state(I2C_MONITOR_CONTROL_STATE_PENDING);

    wait_spins = I2C_MONITOR_CONTROL_WAIT_SPINS;
    while (i2c_monitor_control_load_state() != I2C_MONITOR_CONTROL_STATE_COMPLETE) {
        if (wait_spins == 0u) {
            return I2C_MONITOR_RC_BUSY;
        }

        wait_spins -= 1u;
        i2c_monitor_control_wait_hint();
    }

    i2c_monitor_control_store_state(I2C_MONITOR_CONTROL_STATE_IDLE);
    return g_i2c_monitor_control_mailbox.result;
}

i2c_monitor_rc_t i2c_monitor_control_get_channel_status(uint32_t channel, i2c_monitor_channel_status_t *status_out) {
    uint32_t wait_spins;

    if (g_i2c_monitor_control_inline_mode) {
        if ((g_i2c_monitor_control_get_status_fn == NULL) || (status_out == NULL)) {
            return I2C_MONITOR_RC_INVALID;
        }

        return g_i2c_monitor_control_get_status_fn(channel, status_out);
    }

    if (status_out == NULL) {
        return I2C_MONITOR_RC_INVALID;
    }

    if (!i2c_monitor_control_wait_until_idle()) {
        return I2C_MONITOR_RC_BUSY;
    }

    g_i2c_monitor_control_mailbox.command = I2C_MONITOR_CONTROL_COMMAND_GET_STATUS;
    g_i2c_monitor_control_mailbox.channel = channel;
    i2c_monitor_control_store_state(I2C_MONITOR_CONTROL_STATE_PENDING);

    wait_spins = I2C_MONITOR_CONTROL_WAIT_SPINS;
    while (i2c_monitor_control_load_state() != I2C_MONITOR_CONTROL_STATE_COMPLETE) {
        if (wait_spins == 0u) {
            return I2C_MONITOR_RC_BUSY;
        }

        wait_spins -= 1u;
        i2c_monitor_control_wait_hint();
    }

    *status_out = g_i2c_monitor_control_mailbox.status[0];
    i2c_monitor_control_store_state(I2C_MONITOR_CONTROL_STATE_IDLE);
    return g_i2c_monitor_control_mailbox.result;
}

i2c_monitor_rc_t i2c_monitor_control_get_all_status(i2c_monitor_channel_status_t *status_out) {
    uint32_t wait_spins;

    if (g_i2c_monitor_control_inline_mode) {
        if ((g_i2c_monitor_control_get_all_status_fn == NULL) || (status_out == NULL)) {
            return I2C_MONITOR_RC_INVALID;
        }

        return g_i2c_monitor_control_get_all_status_fn(status_out);
    }

    if (status_out == NULL) {
        return I2C_MONITOR_RC_INVALID;
    }

    if (!i2c_monitor_control_wait_until_idle()) {
        return I2C_MONITOR_RC_BUSY;
    }

    g_i2c_monitor_control_mailbox.command = I2C_MONITOR_CONTROL_COMMAND_GET_ALL_STATUS;
    i2c_monitor_control_store_state(I2C_MONITOR_CONTROL_STATE_PENDING);

    wait_spins = I2C_MONITOR_CONTROL_WAIT_SPINS;
    while (i2c_monitor_control_load_state() != I2C_MONITOR_CONTROL_STATE_COMPLETE) {
        if (wait_spins == 0u) {
            return I2C_MONITOR_RC_BUSY;
        }

        wait_spins -= 1u;
        i2c_monitor_control_wait_hint();
    }

    memcpy(
        status_out,
        g_i2c_monitor_control_mailbox.status,
        I2C_MONITOR_CHANNEL_COUNT * sizeof(g_i2c_monitor_control_mailbox.status[0])
    );
    i2c_monitor_control_store_state(I2C_MONITOR_CONTROL_STATE_IDLE);
    return g_i2c_monitor_control_mailbox.result;
}

bool i2c_monitor_control_poll(void) {
    uint32_t state = i2c_monitor_control_load_state();

    if (state != I2C_MONITOR_CONTROL_STATE_PENDING) {
        return false;
    }

    switch ((i2c_monitor_control_command_t)g_i2c_monitor_control_mailbox.command) {
        case I2C_MONITOR_CONTROL_COMMAND_SET_RATE:
            g_i2c_monitor_control_mailbox.result = (g_i2c_monitor_control_set_channel_fn == NULL)
                ? I2C_MONITOR_RC_FAILED
                : g_i2c_monitor_control_set_channel_fn(
                    g_i2c_monitor_control_mailbox.channel,
                    g_i2c_monitor_control_mailbox.sample_hz
                );
            break;

        case I2C_MONITOR_CONTROL_COMMAND_GET_STATUS:
            g_i2c_monitor_control_mailbox.result = (g_i2c_monitor_control_get_status_fn == NULL)
                ? I2C_MONITOR_RC_INVALID
                : g_i2c_monitor_control_get_status_fn(
                    g_i2c_monitor_control_mailbox.channel,
                    &g_i2c_monitor_control_mailbox.status[0]
                );
            break;

        case I2C_MONITOR_CONTROL_COMMAND_GET_ALL_STATUS:
            g_i2c_monitor_control_mailbox.result = (g_i2c_monitor_control_get_all_status_fn == NULL)
                ? I2C_MONITOR_RC_INVALID
                : g_i2c_monitor_control_get_all_status_fn(g_i2c_monitor_control_mailbox.status);
            break;

        case I2C_MONITOR_CONTROL_COMMAND_NONE:
        default:
            g_i2c_monitor_control_mailbox.result = I2C_MONITOR_RC_INVALID;
            break;
    }

    i2c_monitor_control_store_state(I2C_MONITOR_CONTROL_STATE_COMPLETE);
    return true;
}