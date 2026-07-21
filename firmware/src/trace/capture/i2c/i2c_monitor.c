/**
 * @file i2c_monitor.c
 * @brief Raw I2C sampling pipeline built from one PIO state machine and one DMA lane per channel.
 *
 * The implementation samples four passive I2C channels independently, stores raw packed samples in
 * per-channel staged DMA buffers, and hands completed buffers to the I2C decoder before pushing
 * decoded events into the shared trace ring.
 */

#include "trace/capture/i2c/i2c_monitor.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/sync.h"
#include "pico/time.h"

#include "app_control.h"
#include "config/i2c_monitor_config.h"
#include "trace/decode/i2c/i2c_decoder.h"
#include "trace/decode/i2c/i2c_trace_packet.h"
#include "trace/trace_ring.h"

#include "i2c_monitor.pio.h"

/**
 * @brief Runtime state for one sampled I2C channel.
 *
 * Each channel owns one PIO state machine, one DMA channel, and a small ring of raw DMA buffers.
 */
typedef struct {
    uint8_t kind;
    uint8_t event_type;
    uint8_t event_value;
    uint8_t next_packet_flags;
    uint32_t restart_sample_hz;
} i2c_monitor_pending_transition_t;

typedef struct {
    uint8_t sda_gpio;
    uint8_t scl_gpio;
    uint8_t sm;
    int dma_channel;
    uint8_t active_buffer;
    volatile uint8_t software_owned_buffer_mask;
    volatile uint8_t ready_buffer_mask;
    uint8_t ready_head;
    uint8_t ready_tail;
    uint8_t ready_count;
    uint8_t reserved0;
    bool running;
    bool overrun;
    uint32_t sample_hz;
    uint32_t completed_buffers;
    uint32_t overrun_count;
    volatile i2c_monitor_pending_transition_t pending_transition;
    i2c_decoder_state_t decoder_state;
    i2c_trace_packet_builder_t packet_builder;
    uint8_t ready_queue[I2C_MONITOR_BUFFER_COUNT];
    uint32_t buffers[I2C_MONITOR_BUFFER_COUNT][I2C_MONITOR_BUFFER_WORDS];
} i2c_monitor_channel_state_t;

typedef enum {
    I2C_MONITOR_TRANSITION_NONE = 0u,
    I2C_MONITOR_TRANSITION_STOP = 1u,
    I2C_MONITOR_TRANSITION_RESTART = 2u,
} i2c_monitor_transition_t;

static const PIO i2c_monitor_pio = pio1;
static const uint8_t g_i2c_monitor_logical_channels[I2C_MONITOR_CHANNEL_COUNT] = {
    I2C_MONITOR_CH0_LOGICAL_CHANNEL,
    I2C_MONITOR_CH1_LOGICAL_CHANNEL,
    I2C_MONITOR_CH2_LOGICAL_CHANNEL,
    I2C_MONITOR_CH3_LOGICAL_CHANNEL,
};
static i2c_monitor_channel_state_t g_i2c_monitor_channels[I2C_MONITOR_CHANNEL_COUNT] = {
    {
        .sda_gpio = I2C_MONITOR_CHANNEL0_SDA_GPIO,
        .scl_gpio = I2C_MONITOR_CHANNEL0_SCL_GPIO,
        .sm = 0u,
        .dma_channel = -1,
    },
    {
        .sda_gpio = I2C_MONITOR_CHANNEL1_SDA_GPIO,
        .scl_gpio = I2C_MONITOR_CHANNEL1_SCL_GPIO,
        .sm = 1u,
        .dma_channel = -1,
    },
    {
        .sda_gpio = I2C_MONITOR_CHANNEL2_SDA_GPIO,
        .scl_gpio = I2C_MONITOR_CHANNEL2_SCL_GPIO,
        .sm = 2u,
        .dma_channel = -1,
    },
    {
        .sda_gpio = I2C_MONITOR_CHANNEL3_SDA_GPIO,
        .scl_gpio = I2C_MONITOR_CHANNEL3_SCL_GPIO,
        .sm = 3u,
        .dma_channel = -1,
    },
};
static uint32_t g_i2c_monitor_program_offset;
static bool g_i2c_monitor_initialized;
static bool g_i2c_monitor_init_failed;
static uint8_t g_i2c_monitor_poll_channel_mask;
static uint8_t g_i2c_monitor_transition_channel_mask;

static uint32_t i2c_monitor_timestamp_us(void) {
    return time_us_32();
}

static uint32_t i2c_monitor_channel_index(const i2c_monitor_channel_state_t *channel_state) {
    return (uint32_t)(channel_state - &g_i2c_monitor_channels[0]);
}

static void i2c_monitor_set_poll_interest(
    const i2c_monitor_channel_state_t *channel_state,
    bool poll_needed
) {
    uint8_t channel_mask = (uint8_t)(1u << i2c_monitor_channel_index(channel_state));

    if (poll_needed) {
        g_i2c_monitor_poll_channel_mask = (uint8_t)(g_i2c_monitor_poll_channel_mask | channel_mask);
        return;
    }

    g_i2c_monitor_poll_channel_mask = (uint8_t)(g_i2c_monitor_poll_channel_mask & (uint8_t)~channel_mask);
}

static void i2c_monitor_set_transition_interest(
    const i2c_monitor_channel_state_t *channel_state,
    bool transition_needed
) {
    uint8_t channel_mask = (uint8_t)(1u << i2c_monitor_channel_index(channel_state));

    if (transition_needed) {
        g_i2c_monitor_transition_channel_mask = (uint8_t)(g_i2c_monitor_transition_channel_mask | channel_mask);
        return;
    }

    g_i2c_monitor_transition_channel_mask = (uint8_t)(g_i2c_monitor_transition_channel_mask & (uint8_t)~channel_mask);
}

static void i2c_monitor_clear_pending_transition(i2c_monitor_channel_state_t *channel_state) {
    channel_state->pending_transition.kind = I2C_MONITOR_TRANSITION_NONE;
    channel_state->pending_transition.event_type = 0u;
    channel_state->pending_transition.event_value = 0u;
    channel_state->pending_transition.next_packet_flags = 0u;
    channel_state->pending_transition.restart_sample_hz = 0u;
    i2c_monitor_set_transition_interest(channel_state, false);

    if (!channel_state->running && (channel_state->dma_channel < 0)) {
        i2c_monitor_set_poll_interest(channel_state, false);
    }
}

static bool i2c_monitor_start_channel(
    i2c_monitor_channel_state_t *channel_state,
    uint8_t logical_channel,
    uint32_t sample_hz
);

static bool i2c_monitor_transition_pending(const i2c_monitor_channel_state_t *channel_state);

i2c_monitor_rc_t i2c_monitor_get_all_status(i2c_monitor_channel_status_t *status_out);

static i2c_monitor_channel_state_t *i2c_monitor_get_channel_state(uint32_t channel) {
    if (channel >= I2C_MONITOR_CHANNEL_COUNT) {
        return NULL;
    }

    return &g_i2c_monitor_channels[channel];
}

static void i2c_monitor_reset_channel_capture_state(
    i2c_monitor_channel_state_t *channel_state,
    bool clear_status,
    uint8_t next_packet_flags
) {
    channel_state->software_owned_buffer_mask = 0u;
    channel_state->ready_buffer_mask = 0u;
    channel_state->ready_head = 0u;
    channel_state->ready_tail = 0u;
    channel_state->ready_count = 0u;
    i2c_trace_packet_builder_discard(&channel_state->packet_builder);
    if (next_packet_flags != 0u) {
        i2c_trace_packet_builder_mark_next_packet(&channel_state->packet_builder, next_packet_flags);
    }
    i2c_decoder_init(&channel_state->decoder_state);
    if (clear_status) {
        channel_state->overrun = false;
        channel_state->completed_buffers = 0u;
        channel_state->overrun_count = 0u;
    }
}

static bool i2c_monitor_take_completed_buffer(
    i2c_monitor_channel_state_t *channel_state,
    uint8_t *buffer_index_out
) {
    uint32_t irq_state;
    uint8_t buffer_index;

    irq_state = save_and_disable_interrupts();
    if (channel_state->ready_count == 0u) {
        restore_interrupts(irq_state);
        return false;
    }

    buffer_index = channel_state->ready_queue[channel_state->ready_head];
    channel_state->ready_head = (uint8_t)((channel_state->ready_head + 1u) % I2C_MONITOR_BUFFER_COUNT);
    channel_state->ready_count -= 1u;
    channel_state->ready_buffer_mask = (uint8_t)(channel_state->ready_buffer_mask & (uint8_t)~(1u << buffer_index));
    restore_interrupts(irq_state);

    *buffer_index_out = buffer_index;
    return true;
}

static bool i2c_monitor_enqueue_completed_buffer(
    i2c_monitor_channel_state_t *channel_state,
    uint8_t buffer_index
) {
    if (channel_state->ready_count >= I2C_MONITOR_BUFFER_COUNT) {
        return false;
    }

    channel_state->ready_queue[channel_state->ready_tail] = buffer_index;
    channel_state->ready_tail = (uint8_t)((channel_state->ready_tail + 1u) % I2C_MONITOR_BUFFER_COUNT);
    channel_state->ready_count += 1u;
    channel_state->ready_buffer_mask |= (uint8_t)(1u << buffer_index);
    return true;
}

static bool i2c_monitor_choose_next_dma_buffer(
    const i2c_monitor_channel_state_t *channel_state,
    uint8_t completed_buffer,
    uint8_t *buffer_index_out
) {
    for (uint32_t offset = 1u; offset < I2C_MONITOR_BUFFER_COUNT; ++offset) {
        uint8_t candidate = (uint8_t)((completed_buffer + offset) % I2C_MONITOR_BUFFER_COUNT);

        if ((channel_state->software_owned_buffer_mask & (uint8_t)(1u << candidate)) == 0u) {
            *buffer_index_out = candidate;
            return true;
        }
    }

    return false;
}

static void i2c_monitor_release_completed_buffer(
    i2c_monitor_channel_state_t *channel_state,
    uint8_t buffer_index
) {
    uint32_t irq_state = save_and_disable_interrupts();

    channel_state->software_owned_buffer_mask =
        (uint8_t)(channel_state->software_owned_buffer_mask & (uint8_t)~(1u << buffer_index));
    restore_interrupts(irq_state);
}

static void i2c_monitor_reset_channel_runtime(i2c_monitor_channel_state_t *channel_state) {
    channel_state->active_buffer = 0u;
    i2c_monitor_clear_pending_transition(channel_state);
    channel_state->running = false;
    channel_state->sample_hz = 0u;
    i2c_monitor_reset_channel_capture_state(channel_state, true, 0u);
}

static void i2c_monitor_release_channel_pins(const i2c_monitor_channel_state_t *channel_state) {
    gpio_init(channel_state->sda_gpio);
    gpio_set_dir(channel_state->sda_gpio, GPIO_IN);
    gpio_disable_pulls(channel_state->sda_gpio);
    gpio_set_input_enabled(channel_state->sda_gpio, true);

    gpio_init(channel_state->scl_gpio);
    gpio_set_dir(channel_state->scl_gpio, GPIO_IN);
    gpio_disable_pulls(channel_state->scl_gpio);
    gpio_set_input_enabled(channel_state->scl_gpio, true);
}

static bool i2c_monitor_any_dma_channel_active(void) {
    for (uint32_t channel = 0u; channel < I2C_MONITOR_CHANNEL_COUNT; ++channel) {
        if (g_i2c_monitor_channels[channel].dma_channel >= 0) {
            return true;
        }
    }

    return false;
}

/**
 * @brief Arm DMA to fill one of the two raw sample buffers for a channel.
 * @param channel_state Channel runtime state.
 * @param buffer_index Raw buffer index to become the next DMA target.
 */
static void i2c_monitor_dma_restart(i2c_monitor_channel_state_t *channel_state, uint8_t buffer_index) {
    dma_channel_set_write_addr(
        channel_state->dma_channel,
        channel_state->buffers[buffer_index],
        false
    );
    dma_channel_set_trans_count(
        channel_state->dma_channel,
        I2C_MONITOR_BUFFER_WORDS,
        true
    );
    channel_state->active_buffer = buffer_index;
}

/**
 * @brief DMA completion handler for all configured I2C sampler channels.
 *
 * The handler immediately re-arms DMA on the alternate buffer and marks the completed slot ready
 * for poll-time decode using caller-owned channel state inside the monitor.
 */
static bool i2c_monitor_push_trace_packet(void *context, const trace_packet_t *packet) {
    (void)context;
    return trace_ring_push(packet);
}

static void i2c_monitor_stop_channel_hardware(i2c_monitor_channel_state_t *channel_state) {
    uint32_t channel_mask;

    if (channel_state->dma_channel < 0) {
        i2c_monitor_release_channel_pins(channel_state);
        channel_state->running = false;
        return;
    }

    channel_mask = 1u << (uint32_t)channel_state->dma_channel;
    dma_channel_set_irq0_enabled((uint)channel_state->dma_channel, false);
    dma_channel_abort((uint)channel_state->dma_channel);
    dma_hw->ints0 = channel_mask;
    dma_channel_unclaim((uint)channel_state->dma_channel);
    pio_sm_set_enabled(i2c_monitor_pio, channel_state->sm, false);
    pio_sm_clear_fifos(i2c_monitor_pio, channel_state->sm);
    pio_sm_restart(i2c_monitor_pio, channel_state->sm);
    i2c_monitor_release_channel_pins(channel_state);

    channel_state->dma_channel = -1;
    channel_state->running = false;

    if (!i2c_monitor_transition_pending(channel_state)) {
        i2c_monitor_set_poll_interest(channel_state, false);
    }

    if (!i2c_monitor_any_dma_channel_active()) {
        irq_set_enabled(DMA_IRQ_0, false);
    }
}

static void i2c_monitor_shutdown_channel(i2c_monitor_channel_state_t *channel_state) {
    i2c_monitor_stop_channel_hardware(channel_state);
    i2c_monitor_reset_channel_runtime(channel_state);
}
static void i2c_monitor_shutdown_all(void) {
    irq_set_enabled(DMA_IRQ_0, false);

    for (uint32_t channel = 0u; channel < I2C_MONITOR_CHANNEL_COUNT; ++channel) {
        i2c_monitor_shutdown_channel(&g_i2c_monitor_channels[channel]);
    }

    pio_remove_program(i2c_monitor_pio, &i2c_monitor_sampler_program, g_i2c_monitor_program_offset);
}

static bool i2c_monitor_emit_boundary_event(
    i2c_monitor_channel_state_t *channel_state,
    uint8_t event_type,
    uint8_t event_value
) {
    return i2c_trace_packet_builder_append_event(
        &channel_state->packet_builder,
        event_type,
        event_value
    );
}

static void i2c_monitor_drop_channel_backlog(
    i2c_monitor_channel_state_t *channel_state,
    uint8_t next_packet_flags
) {
    i2c_monitor_reset_channel_capture_state(channel_state, false, next_packet_flags);
}

static bool i2c_monitor_transition_pending(const i2c_monitor_channel_state_t *channel_state) {
    return channel_state->pending_transition.kind != I2C_MONITOR_TRANSITION_NONE;
}

static void i2c_monitor_fill_channel_status(
    const i2c_monitor_channel_state_t *channel_state,
    i2c_monitor_channel_status_t *status_out
) {
    status_out->initialized = g_i2c_monitor_initialized && !g_i2c_monitor_init_failed;
    status_out->running = channel_state->running;
    status_out->overrun = channel_state->overrun;
    status_out->transition_pending = i2c_monitor_transition_pending(channel_state);
    status_out->transition_reason = status_out->transition_pending
        ? channel_state->pending_transition.event_type
        : 0u;
    status_out->sample_hz = channel_state->sample_hz;
    status_out->completed_buffers = channel_state->completed_buffers;
    status_out->overrun_count = channel_state->overrun_count;
}

static void i2c_monitor_latch_transition(
    i2c_monitor_channel_state_t *channel_state,
    uint8_t next_transition,
    uint8_t event_type,
    uint8_t event_value,
    uint32_t restart_sample_hz,
    uint8_t next_packet_flags
) {
    channel_state->pending_transition.event_type = event_type;
    channel_state->pending_transition.event_value = event_value;
    channel_state->pending_transition.next_packet_flags = next_packet_flags;
    channel_state->pending_transition.restart_sample_hz = restart_sample_hz;
    channel_state->pending_transition.kind = next_transition;
    i2c_monitor_set_transition_interest(channel_state, true);
    i2c_monitor_set_poll_interest(channel_state, true);
}

static bool i2c_monitor_take_next_transition(
    i2c_monitor_channel_state_t *channel_state,
    i2c_monitor_pending_transition_t *pending_transition_out
) {
    uint32_t irq_state = save_and_disable_interrupts();

    if (!i2c_monitor_transition_pending(channel_state)) {
        restore_interrupts(irq_state);
        return false;
    }

    *pending_transition_out = channel_state->pending_transition;
    i2c_monitor_clear_pending_transition(channel_state);
    restore_interrupts(irq_state);
    return true;
}

static void i2c_monitor_transition_channel(
    i2c_monitor_channel_state_t *channel_state,
    uint8_t logical_channel,
    uint8_t event_type,
    uint8_t event_value,
    uint32_t restart_sample_hz,
    uint8_t next_packet_flags,
    bool retain_status_on_restart
) {
    bool emitted_event = true;
    bool restart_started = false;
    bool restored_overrun = channel_state->overrun;
    uint32_t restored_overrun_count = channel_state->overrun_count;
    bool restore_status_after_restart = retain_status_on_restart;

    i2c_monitor_stop_channel_hardware(channel_state);

    if (event_type != 0u) {
        emitted_event = i2c_monitor_emit_boundary_event(channel_state, event_type, event_value);
        if (!emitted_event) {
            restored_overrun = true;
            restored_overrun_count += 1u;
            next_packet_flags |= TRACE_FLAG_OVERFLOW;
            restore_status_after_restart = (restart_sample_hz != 0u);
        }
    }

    i2c_monitor_reset_channel_runtime(channel_state);
    if (restart_sample_hz != 0u) {
        restart_started = i2c_monitor_start_channel(channel_state, logical_channel, restart_sample_hz);
        if (restart_started && (next_packet_flags != 0u)) {
            i2c_trace_packet_builder_mark_next_packet(&channel_state->packet_builder, next_packet_flags);
        }
    }

    if (restart_started && restore_status_after_restart) {
        channel_state->overrun = restored_overrun;
        channel_state->overrun_count = restored_overrun_count;
    }
}

static void i2c_monitor_process_decode_result(
    i2c_monitor_channel_state_t *channel_state,
    uint8_t logical_channel,
    i2c_decoder_result_t result
) {
    switch (result) {
        case I2C_DECODER_RESULT_OK:
            break;

        case I2C_DECODER_RESULT_SINK_REJECTED:
            channel_state->overrun = true;
            channel_state->overrun_count += 1u;
            i2c_monitor_drop_channel_backlog(channel_state, TRACE_FLAG_OVERFLOW);
            break;

        case I2C_DECODER_RESULT_INVALID_INPUT:
        default:
            i2c_monitor_transition_channel(
                channel_state,
                logical_channel,
                I2C_DECODE_EVENT_ERROR,
                (uint8_t)result,
                channel_state->sample_hz,
                0u,
                false
            );
            break;
    }
}

static void i2c_monitor_process_pending_transition(
    i2c_monitor_channel_state_t *channel_state,
    uint8_t logical_channel
) {
    i2c_monitor_pending_transition_t pending_transition;

    if (!i2c_monitor_take_next_transition(
            channel_state,
            &pending_transition
        )) {
        return;
    }

    if ((pending_transition.kind == I2C_MONITOR_TRANSITION_STOP) || !app_control_stream_enabled()) {
        pending_transition.restart_sample_hz = 0u;
    }

    i2c_monitor_transition_channel(
        channel_state,
        logical_channel,
        pending_transition.event_type,
        pending_transition.event_value,
        pending_transition.restart_sample_hz,
        pending_transition.next_packet_flags,
        true
    );
}

static void i2c_monitor_schedule_stream_stop_transition(i2c_monitor_channel_state_t *channel_state) {
    if (i2c_monitor_transition_pending(channel_state) || (channel_state->dma_channel < 0)) {
        return;
    }

    i2c_monitor_latch_transition(
        channel_state,
        I2C_MONITOR_TRANSITION_STOP,
        I2C_DECODE_EVENT_CONTROL_STOP,
        0u,
        0u,
        0u
    );
}

static void i2c_monitor_dma_irq_handler(void) {
    uint32_t pending = dma_hw->ints0;
    bool stream_enabled = app_control_stream_enabled();

    /* This monitor owns the exclusive DMA IRQ0 handler, so acknowledge every latched bit up front.
     * Otherwise an unrelated stale pending bit can retrigger the IRQ forever and starve control work.
     */
    dma_hw->ints0 = pending;

    for (uint32_t channel = 0u; channel < I2C_MONITOR_CHANNEL_COUNT; ++channel) {
        i2c_monitor_channel_state_t *channel_state = &g_i2c_monitor_channels[channel];
        uint32_t channel_mask;
        uint8_t completed_buffer;
        uint8_t next_buffer;

        if (channel_state->dma_channel < 0) {
            continue;
        }

        channel_mask = 1u << (uint32_t)channel_state->dma_channel;
        if ((pending & channel_mask) == 0u) {
            continue;
        }

        completed_buffer = channel_state->active_buffer;

        if (!stream_enabled || i2c_monitor_transition_pending(channel_state)) {
            continue;
        }

        if (!i2c_monitor_choose_next_dma_buffer(channel_state, completed_buffer, &next_buffer)) {
            channel_state->overrun = true;
            channel_state->overrun_count += 1u;
            i2c_monitor_drop_channel_backlog(channel_state, TRACE_FLAG_OVERFLOW);
            next_buffer = (uint8_t)((completed_buffer + 1u) % I2C_MONITOR_BUFFER_COUNT);
            i2c_monitor_dma_restart(channel_state, next_buffer);
            continue;
        }

        i2c_monitor_dma_restart(channel_state, next_buffer);
        channel_state->software_owned_buffer_mask |= (uint8_t)(1u << completed_buffer);
        if (!i2c_monitor_enqueue_completed_buffer(channel_state, completed_buffer)) {
            channel_state->overrun = true;
            channel_state->overrun_count += 1u;
            i2c_monitor_drop_channel_backlog(channel_state, TRACE_FLAG_OVERFLOW);
            continue;
        }
        channel_state->completed_buffers += 1u;
    }
}

void i2c_monitor_poll(void) {
    uint8_t active_mask;
    uint8_t pending_mask;
    bool stream_enabled;

    if (!g_i2c_monitor_initialized || g_i2c_monitor_init_failed) {
        return;
    }

    stream_enabled = app_control_stream_enabled();
    active_mask = g_i2c_monitor_poll_channel_mask;

    if (!stream_enabled) {
        for (uint32_t channel = 0u; channel < I2C_MONITOR_CHANNEL_COUNT; ++channel) {
            if ((active_mask & (uint8_t)(1u << channel)) == 0u) {
                continue;
            }

            i2c_monitor_schedule_stream_stop_transition(&g_i2c_monitor_channels[channel]);
        }
    }

    pending_mask = g_i2c_monitor_transition_channel_mask;
    for (uint32_t channel = 0u; channel < I2C_MONITOR_CHANNEL_COUNT; ++channel) {
        if ((pending_mask & (uint8_t)(1u << channel)) == 0u) {
            continue;
        }

        i2c_monitor_process_pending_transition(
            &g_i2c_monitor_channels[channel],
            g_i2c_monitor_logical_channels[channel]
        );
    }

    if (!stream_enabled) {
        return;
    }

    active_mask = (uint8_t)(g_i2c_monitor_poll_channel_mask & (uint8_t)~g_i2c_monitor_transition_channel_mask);
    for (uint32_t channel = 0u; channel < I2C_MONITOR_CHANNEL_COUNT; ++channel) {
        i2c_monitor_channel_state_t *channel_state;
        uint32_t drained_buffers;

        if ((active_mask & (uint8_t)(1u << channel)) == 0u) {
            continue;
        }

        channel_state = &g_i2c_monitor_channels[channel];
        if (channel_state->dma_channel < 0) {
            continue;
        }

        /* Drain every queued completion for this channel before moving on so bursty traffic does not
         * linger in software-owned buffers across multiple producer polls.
         */
        for (drained_buffers = 0u; drained_buffers < I2C_MONITOR_BUFFER_COUNT; ++drained_buffers) {
            uint8_t completed_buffer;

            if (!i2c_monitor_take_completed_buffer(channel_state, &completed_buffer)) {
                break;
            }

            i2c_monitor_process_decode_result(
                channel_state,
                g_i2c_monitor_logical_channels[channel],
                i2c_decoder_process_buffer_into_builder(
                    &channel_state->decoder_state,
                    channel_state->buffers[completed_buffer],
                    I2C_MONITOR_BUFFER_WORDS,
                    &channel_state->packet_builder
                )
            );
            i2c_monitor_release_completed_buffer(channel_state, completed_buffer);

            if (i2c_monitor_transition_pending(channel_state) || (channel_state->dma_channel < 0)) {
                break;
            }
        }
    }
}

/**
 * @brief Configure GPIO, PIO, and DMA resources for one I2C sample channel.
 * @param channel_state Channel runtime state to initialize.
 */
static bool i2c_monitor_start_channel(
    i2c_monitor_channel_state_t *channel_state,
    uint8_t logical_channel,
    uint32_t sample_hz
) {
    dma_channel_config dma_config;
    pio_sm_config sm_config;
    float clkdiv;
    int dma_channel;

    if (sample_hz == 0u) {
        return false;
    }

    if (!i2c_trace_packet_builder_init(
            &channel_state->packet_builder,
            logical_channel,
            i2c_monitor_push_trace_packet,
            NULL,
            i2c_monitor_timestamp_us
        )) {
        return false;
    }

    gpio_disable_pulls(channel_state->sda_gpio);
    gpio_disable_pulls(channel_state->scl_gpio);
    gpio_set_input_enabled(channel_state->sda_gpio, true);
    gpio_set_input_enabled(channel_state->scl_gpio, true);

    sm_config = i2c_monitor_sampler_program_get_default_config(g_i2c_monitor_program_offset);
    sm_config_set_in_pins(&sm_config, channel_state->sda_gpio);
    sm_config_set_in_shift(&sm_config, false, true, 32u);
    clkdiv = (float)clock_get_hz(clk_sys) / (float)sample_hz;
    if (clkdiv < 1.0f) {
        clkdiv = 1.0f;
    }

    i2c_monitor_sampler_program_init(
        i2c_monitor_pio,
        channel_state->sm,
        g_i2c_monitor_program_offset,
        channel_state->sda_gpio,
        clkdiv
    );

    dma_channel = dma_claim_unused_channel(false);
    if (dma_channel < 0) {
        pio_sm_set_enabled(i2c_monitor_pio, channel_state->sm, false);
        pio_sm_clear_fifos(i2c_monitor_pio, channel_state->sm);
        pio_sm_restart(i2c_monitor_pio, channel_state->sm);
        i2c_monitor_release_channel_pins(channel_state);
        return false;
    }

    channel_state->dma_channel = dma_channel;
    dma_config = dma_channel_get_default_config((uint)channel_state->dma_channel);
    channel_config_set_transfer_data_size(&dma_config, DMA_SIZE_32);
    channel_config_set_dreq(&dma_config, pio_get_dreq(i2c_monitor_pio, channel_state->sm, false));
    channel_config_set_read_increment(&dma_config, false);
    channel_config_set_write_increment(&dma_config, true);

    dma_channel_configure(
        (uint)channel_state->dma_channel,
        &dma_config,
        channel_state->buffers[0],
        &i2c_monitor_pio->rxf[channel_state->sm],
        I2C_MONITOR_BUFFER_WORDS,
        false
    );
    dma_hw->ints0 = 1u << (uint32_t)channel_state->dma_channel;
    dma_channel_set_irq0_enabled((uint)channel_state->dma_channel, true);
    irq_set_enabled(DMA_IRQ_0, true);

    i2c_monitor_reset_channel_runtime(channel_state);
    channel_state->running = true;
    channel_state->sample_hz = sample_hz;
    i2c_monitor_set_poll_interest(channel_state, true);
    i2c_monitor_dma_restart(channel_state, 0u);
    return true;
}

/** @copydoc i2c_monitor_init */
i2c_monitor_rc_t i2c_monitor_init(void) {
    uint32_t channel;
    if (g_i2c_monitor_initialized) {
        return I2C_MONITOR_RC_OK;
    }

    if (g_i2c_monitor_init_failed) {
        return I2C_MONITOR_RC_FAILED;
    }

    if (!pio_can_add_program(i2c_monitor_pio, &i2c_monitor_sampler_program)) {
        g_i2c_monitor_init_failed = true;
        return I2C_MONITOR_RC_FAILED;
    }

    g_i2c_monitor_program_offset = pio_add_program(i2c_monitor_pio, &i2c_monitor_sampler_program);

    irq_set_exclusive_handler(DMA_IRQ_0, i2c_monitor_dma_irq_handler);
    irq_set_enabled(DMA_IRQ_0, false);

    for (channel = 0u; channel < I2C_MONITOR_CHANNEL_COUNT; ++channel) {
        i2c_monitor_release_channel_pins(&g_i2c_monitor_channels[channel]);
        i2c_monitor_reset_channel_runtime(&g_i2c_monitor_channels[channel]);
    }

    g_i2c_monitor_poll_channel_mask = 0u;
    g_i2c_monitor_transition_channel_mask = 0u;
    g_i2c_monitor_initialized = true;
    return I2C_MONITOR_RC_OK;
}

bool i2c_monitor_needs_poll(void) {
    return g_i2c_monitor_initialized && !g_i2c_monitor_init_failed && (g_i2c_monitor_poll_channel_mask != 0u);
}

i2c_monitor_rc_t i2c_monitor_set_channel_sample_hz(uint32_t channel, uint32_t sample_hz) {
    i2c_monitor_channel_state_t *channel_state = i2c_monitor_get_channel_state(channel);

    if ((channel_state == NULL) || !g_i2c_monitor_initialized || g_i2c_monitor_init_failed) {
        return I2C_MONITOR_RC_INVALID;
    }

    if (i2c_monitor_transition_pending(channel_state)) {
        return I2C_MONITOR_RC_BUSY;
    }

    if (!app_control_stream_enabled() && (sample_hz != 0u)) {
        return I2C_MONITOR_RC_DISABLED;
    }

    if (channel_state->running) {
        i2c_monitor_transition_channel(
            channel_state,
            g_i2c_monitor_logical_channels[channel],
            (sample_hz == 0u) ? I2C_DECODE_EVENT_CONTROL_STOP : I2C_DECODE_EVENT_CONTROL_RECONFIG,
            0u,
            sample_hz,
            0u,
            false
        );
        return ((sample_hz == 0u) || channel_state->running)
            ? I2C_MONITOR_RC_OK
            : I2C_MONITOR_RC_FAILED;
    }

    if (sample_hz == 0u) {
        return I2C_MONITOR_RC_OK;
    }

    return i2c_monitor_start_channel(channel_state, g_i2c_monitor_logical_channels[channel], sample_hz)
        ? I2C_MONITOR_RC_OK
        : I2C_MONITOR_RC_FAILED;
}

i2c_monitor_rc_t i2c_monitor_get_channel_status(uint32_t channel, i2c_monitor_channel_status_t *status_out) {
    i2c_monitor_channel_state_t *channel_state = i2c_monitor_get_channel_state(channel);

    if ((channel_state == NULL) || (status_out == NULL)) {
        return I2C_MONITOR_RC_INVALID;
    }

    i2c_monitor_fill_channel_status(channel_state, status_out);
    return I2C_MONITOR_RC_OK;
}

i2c_monitor_rc_t i2c_monitor_get_all_status(i2c_monitor_channel_status_t *status_out) {
    if (status_out == NULL) {
        return I2C_MONITOR_RC_INVALID;
    }

    for (uint32_t channel = 0u; channel < I2C_MONITOR_CHANNEL_COUNT; ++channel) {
        i2c_monitor_fill_channel_status(&g_i2c_monitor_channels[channel], &status_out[channel]);
    }

    return I2C_MONITOR_RC_OK;
}

