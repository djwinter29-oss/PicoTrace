/**
 * @file i2c_monitor.c
 * @brief Raw I2C sampling pipeline built from one PIO state machine and one DMA lane per channel.
 *
 * The implementation samples four passive I2C channels independently, stores raw packed samples in
 * per-channel ping-pong DMA buffers, and hands completed buffers to the I2C decoder before pushing
 * decoded events into the shared trace ring.
 */

#include "trace/capture/i2c_monitor.h"

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
#include "trace/decode/i2c_decoder.h"
#include "trace/decode/i2c_trace_packet.h"
#include "trace/trace_ring.h"

#include "i2c_monitor.pio.h"

/**
 * @brief Runtime state for one sampled I2C channel.
 *
 * Each channel owns one PIO state machine, one DMA channel, and two alternating raw buffers.
 */
typedef struct {
    uint8_t sda_gpio;
    uint8_t scl_gpio;
    uint8_t sm;
    int dma_channel;
    uint8_t active_buffer;
    volatile bool staged_buffer_ready;
    bool running;
    bool overrun;
    uint32_t sample_hz;
    uint32_t completed_buffers;
    uint32_t overrun_count;
    i2c_decoder_state_t decoder_state;
    i2c_trace_packet_builder_t packet_builder;
    uint32_t staged_buffer[I2C_MONITOR_BUFFER_WORDS];
    uint32_t buffers[I2C_MONITOR_BUFFER_COUNT][I2C_MONITOR_BUFFER_WORDS];
} i2c_monitor_channel_state_t;

static const PIO i2c_monitor_pio = pio0;
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

static uint32_t i2c_monitor_timestamp_us(void) {
    return time_us_32();
}

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
    channel_state->staged_buffer_ready = false;
    memset(channel_state->staged_buffer, 0, sizeof(channel_state->staged_buffer));
    memset(channel_state->buffers, 0, sizeof(channel_state->buffers));
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

static bool i2c_monitor_take_staged_buffer(
    i2c_monitor_channel_state_t *channel_state,
    uint32_t *raw_words,
    uint32_t raw_word_count
) {
    uint32_t irq_state;

    irq_state = save_and_disable_interrupts();
    if (!channel_state->staged_buffer_ready) {
        restore_interrupts(irq_state);
        return false;
    }

    memcpy(raw_words, channel_state->staged_buffer, raw_word_count * sizeof(raw_words[0]));
    channel_state->staged_buffer_ready = false;
    restore_interrupts(irq_state);
    return true;
}

static void i2c_monitor_reset_channel_runtime(i2c_monitor_channel_state_t *channel_state) {
    channel_state->active_buffer = 0u;
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

/**
 * @brief Arm DMA to fill one of the two raw sample buffers for a channel.
 * @param channel_state Channel runtime state.
 * @param buffer_index Ping-pong buffer index to become the next DMA target.
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
 * The handler immediately re-arms DMA on the alternate buffer, then decodes the completed ping-
 * pong buffer using caller-owned channel state inside the monitor.
 */
static bool i2c_monitor_push_trace_packet(void *context, const trace_packet_t *packet) {
    (void)context;
    return trace_ring_push(packet);
}

static void i2c_monitor_shutdown_channel(i2c_monitor_channel_state_t *channel_state) {
    uint32_t channel_mask;

    if (channel_state->dma_channel < 0) {
        i2c_monitor_release_channel_pins(channel_state);
        i2c_monitor_reset_channel_runtime(channel_state);
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

static void i2c_monitor_restart_channel(
    i2c_monitor_channel_state_t *channel_state,
    uint8_t logical_channel,
    uint8_t restart_event_type,
    uint8_t restart_event_value,
    uint8_t next_packet_flags
) {
    bool overrun = channel_state->overrun;
    uint32_t overrun_count = channel_state->overrun_count;
    uint32_t sample_hz = channel_state->sample_hz;

    if (restart_event_type != 0u) {
        if (!i2c_monitor_emit_boundary_event(channel_state, restart_event_type, restart_event_value)) {
            overrun = true;
            overrun_count += 1u;
            next_packet_flags |= TRACE_FLAG_OVERFLOW;
        }
    }

    i2c_monitor_shutdown_channel(channel_state);
    if ((sample_hz != 0u)
            && i2c_monitor_start_channel(channel_state, logical_channel, sample_hz)
            && (next_packet_flags != 0u)) {
        i2c_trace_packet_builder_mark_next_packet(&channel_state->packet_builder, next_packet_flags);
    }

    channel_state->overrun = overrun;
    channel_state->overrun_count = overrun_count;
}

static void i2c_monitor_handle_decode_result(
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
            i2c_monitor_restart_channel(
                channel_state,
                logical_channel,
                0u,
                0u,
                TRACE_FLAG_OVERFLOW
            );
            break;

        case I2C_DECODER_RESULT_INVALID_INPUT:
        default: {
            bool emitted_error = i2c_monitor_emit_boundary_event(
                channel_state,
                I2C_DECODE_EVENT_ERROR,
                (uint8_t)result
            );

            if (!emitted_error) {
                channel_state->overrun = true;
                channel_state->overrun_count += 1u;
            }

            i2c_monitor_reset_channel_capture_state(
                channel_state,
                false,
                emitted_error ? 0u : TRACE_FLAG_OVERFLOW
            );
            break;
        }
    }
}

static void i2c_monitor_dma_irq_handler(void) {
    uint32_t pending = dma_hw->ints0;
    bool stream_enabled = app_control_stream_enabled();

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

        dma_hw->ints0 = channel_mask;

        completed_buffer = channel_state->active_buffer;
        next_buffer = (uint8_t)(completed_buffer ^ 1u);
        i2c_monitor_dma_restart(channel_state, next_buffer);

        if (!stream_enabled) {
            continue;
        }

        if (channel_state->staged_buffer_ready) {
            channel_state->overrun = true;
            channel_state->overrun_count += 1u;
            i2c_monitor_restart_channel(
                channel_state,
                g_i2c_monitor_logical_channels[channel],
                I2C_DECODE_EVENT_OVERFLOW,
                0u,
                TRACE_FLAG_OVERFLOW
            );
            continue;
        }

        memcpy(
            channel_state->staged_buffer,
            channel_state->buffers[completed_buffer],
            sizeof(channel_state->staged_buffer)
        );
        channel_state->staged_buffer_ready = true;
        channel_state->completed_buffers += 1u;
    }
}

void i2c_monitor_poll(void) {
    uint32_t staged_buffer[I2C_MONITOR_BUFFER_WORDS];
    bool stream_enabled;

    if (!g_i2c_monitor_initialized || g_i2c_monitor_init_failed) {
        return;
    }

    stream_enabled = app_control_stream_enabled();
    if (!stream_enabled) {
        for (uint32_t channel = 0u; channel < I2C_MONITOR_CHANNEL_COUNT; ++channel) {
            i2c_monitor_channel_state_t *channel_state = &g_i2c_monitor_channels[channel];

            if (channel_state->dma_channel >= 0) {
                (void)i2c_monitor_emit_boundary_event(channel_state, I2C_DECODE_EVENT_CONTROL_STOP, 0u);
                i2c_monitor_shutdown_channel(channel_state);
            }
        }
        return;
    }

    for (uint32_t channel = 0u; channel < I2C_MONITOR_CHANNEL_COUNT; ++channel) {
        i2c_monitor_channel_state_t *channel_state = &g_i2c_monitor_channels[channel];

        if (channel_state->dma_channel < 0) {
            continue;
        }

        if (!i2c_monitor_take_staged_buffer(
                channel_state,
                staged_buffer,
                I2C_MONITOR_BUFFER_WORDS
            )) {
            continue;
        }

        i2c_monitor_handle_decode_result(
            channel_state,
            g_i2c_monitor_logical_channels[channel],
            i2c_decoder_process_buffer(
                &channel_state->decoder_state,
                staged_buffer,
                I2C_MONITOR_BUFFER_WORDS,
                i2c_trace_packet_builder_capture_event,
                &channel_state->packet_builder
            )
        );
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

    i2c_monitor_reset_channel_runtime(channel_state);
    channel_state->running = true;
    channel_state->sample_hz = sample_hz;
    i2c_monitor_dma_restart(channel_state, 0u);
    return true;
}

/** @copydoc i2c_monitor_init */
bool i2c_monitor_init(void) {
    uint32_t channel;
    if (g_i2c_monitor_initialized) {
        return true;
    }

    if (g_i2c_monitor_init_failed) {
        return false;
    }

    if (!pio_can_add_program(i2c_monitor_pio, &i2c_monitor_sampler_program)) {
        g_i2c_monitor_init_failed = true;
        return false;
    }

    g_i2c_monitor_program_offset = pio_add_program(i2c_monitor_pio, &i2c_monitor_sampler_program);

    irq_set_exclusive_handler(DMA_IRQ_0, i2c_monitor_dma_irq_handler);
    irq_set_enabled(DMA_IRQ_0, true);

    for (channel = 0u; channel < I2C_MONITOR_CHANNEL_COUNT; ++channel) {
        i2c_monitor_release_channel_pins(&g_i2c_monitor_channels[channel]);
        i2c_monitor_reset_channel_runtime(&g_i2c_monitor_channels[channel]);
    }

    g_i2c_monitor_initialized = true;
    return true;
}

bool i2c_monitor_set_channel_sample_hz(uint32_t channel, uint32_t sample_hz) {
    i2c_monitor_channel_state_t *channel_state = i2c_monitor_get_channel_state(channel);
    bool was_running;

    if ((channel_state == NULL) || !g_i2c_monitor_initialized || g_i2c_monitor_init_failed) {
        return false;
    }

    was_running = channel_state->running;
    if (was_running) {
        uint8_t boundary_event = (sample_hz == 0u)
            ? I2C_DECODE_EVENT_CONTROL_STOP
            : I2C_DECODE_EVENT_CONTROL_RECONFIG;

        if (!i2c_monitor_emit_boundary_event(channel_state, boundary_event, 0u)) {
            channel_state->overrun = true;
            channel_state->overrun_count += 1u;
            i2c_trace_packet_builder_mark_next_packet(&channel_state->packet_builder, TRACE_FLAG_OVERFLOW);
        }
    }

    i2c_monitor_shutdown_channel(channel_state);
    if (sample_hz == 0u) {
        return true;
    }

    return i2c_monitor_start_channel(channel_state, g_i2c_monitor_logical_channels[channel], sample_hz);
}

bool i2c_monitor_get_channel_status(uint32_t channel, i2c_monitor_channel_status_t *status_out) {
    i2c_monitor_channel_state_t *channel_state = i2c_monitor_get_channel_state(channel);

    if ((channel_state == NULL) || (status_out == NULL)) {
        return false;
    }

    status_out->initialized = g_i2c_monitor_initialized && !g_i2c_monitor_init_failed;
    status_out->running = channel_state->running;
    status_out->overrun = channel_state->overrun;
    status_out->sample_hz = channel_state->sample_hz;
    status_out->completed_buffers = channel_state->completed_buffers;
    status_out->overrun_count = channel_state->overrun_count;
    return true;
}

