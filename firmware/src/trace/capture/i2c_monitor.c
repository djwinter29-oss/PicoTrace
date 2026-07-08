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
#include "trace/trace_packet.h"
#include "trace/trace_ring.h"

#include "i2c_monitor.pio.h"

/**
 * @brief Runtime state for one sampled I2C channel.
 *
 * Each channel owns one PIO state machine, one DMA channel, and two alternating raw buffers.
 */
typedef struct {
    uint8_t logical_channel;
    uint8_t sda_gpio;
    uint8_t scl_gpio;
    uint8_t sm;
    int dma_channel;
    uint8_t active_buffer;
    uint32_t sample_hz;
    uint32_t completed_buffers;
    uint32_t overrun_count;
    uint32_t emitted_packets;
    i2c_decoder_state_t decoder_state;
    trace_packet_t packet;
    bool packet_open;
    bool transaction_fragmented;
    uint32_t packet_payload_offset;
    uint16_t packet_event_count;
    uint32_t buffers[I2C_MONITOR_BUFFER_COUNT][I2C_MONITOR_BUFFER_WORDS];
} i2c_monitor_channel_state_t;

static const PIO i2c_monitor_pio = pio0;
static i2c_monitor_channel_state_t g_i2c_monitor_channels[I2C_MONITOR_CHANNEL_COUNT] = {
    {
        .logical_channel = I2C_MONITOR_CH0_LOGICAL_CHANNEL,
        .sda_gpio = I2C_MONITOR_CHANNEL0_SDA_GPIO,
        .scl_gpio = I2C_MONITOR_CHANNEL0_SCL_GPIO,
        .sm = 0u,
        .dma_channel = -1,
        .sample_hz = I2C_MONITOR_CH0_SAMPLE_HZ,
    },
    {
        .logical_channel = I2C_MONITOR_CH1_LOGICAL_CHANNEL,
        .sda_gpio = I2C_MONITOR_CHANNEL1_SDA_GPIO,
        .scl_gpio = I2C_MONITOR_CHANNEL1_SCL_GPIO,
        .sm = 1u,
        .dma_channel = -1,
        .sample_hz = I2C_MONITOR_CH1_SAMPLE_HZ,
    },
    {
        .logical_channel = I2C_MONITOR_CH2_LOGICAL_CHANNEL,
        .sda_gpio = I2C_MONITOR_CHANNEL2_SDA_GPIO,
        .scl_gpio = I2C_MONITOR_CHANNEL2_SCL_GPIO,
        .sm = 2u,
        .dma_channel = -1,
        .sample_hz = I2C_MONITOR_CH2_SAMPLE_HZ,
    },
    {
        .logical_channel = I2C_MONITOR_CH3_LOGICAL_CHANNEL,
        .sda_gpio = I2C_MONITOR_CHANNEL3_SDA_GPIO,
        .scl_gpio = I2C_MONITOR_CHANNEL3_SCL_GPIO,
        .sm = 3u,
        .dma_channel = -1,
        .sample_hz = I2C_MONITOR_CH3_SAMPLE_HZ,
    },
};
static uint32_t g_i2c_monitor_program_offset;
static bool g_i2c_monitor_initialized;

typedef struct {
    i2c_monitor_channel_state_t *channel_state;
} i2c_monitor_emit_context_t;

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
static void i2c_monitor_begin_packet(i2c_monitor_emit_context_t *context) {
    i2c_monitor_channel_state_t *channel_state = context->channel_state;

    channel_state->packet.header.version = TRACE_PACKET_VERSION;
    channel_state->packet.header.type = TRACE_TYPE_I2C;
    channel_state->packet.header.channel = channel_state->logical_channel;
    channel_state->packet.header.flags = 0u;
    channel_state->packet.header.payload_len = 0u;
    channel_state->packet.header.meta = 0u;
    channel_state->packet.header.sequence = ++channel_state->emitted_packets;
    channel_state->packet.header.timestamp_us = 0u;
    if (channel_state->transaction_fragmented) {
        channel_state->packet.header.flags |= TRACE_FLAG_CONTINUED;
    }
    channel_state->packet_payload_offset = 0u;
    channel_state->packet_event_count = 0u;
    channel_state->packet_open = true;
}

static void i2c_monitor_reset_packet_state(i2c_monitor_channel_state_t *channel_state) {
    channel_state->packet_open = false;
    channel_state->transaction_fragmented = false;
    channel_state->packet_payload_offset = 0u;
    channel_state->packet_event_count = 0u;
}

static bool i2c_monitor_flush_packet(i2c_monitor_emit_context_t *context, bool end_of_transaction) {
    i2c_monitor_channel_state_t *channel_state = context->channel_state;

    if (!channel_state->packet_open) {
        return true;
    }

    if (end_of_transaction) {
        channel_state->packet.header.flags |= TRACE_FLAG_END;
    }
    channel_state->packet.header.payload_len = (uint16_t)channel_state->packet_payload_offset;
    channel_state->packet.header.meta = channel_state->packet_event_count;
    if (!trace_ring_push(&channel_state->packet)) {
        i2c_monitor_reset_packet_state(channel_state);
        return false;
    }

    i2c_monitor_reset_packet_state(channel_state);
    channel_state->transaction_fragmented = !end_of_transaction;
    return true;
}

static bool i2c_monitor_emit_event(void *context_ptr, uint8_t event_type, uint8_t event_value) {
    i2c_monitor_emit_context_t *context = (i2c_monitor_emit_context_t *)context_ptr;
    i2c_monitor_channel_state_t *channel_state = context->channel_state;

    if (!channel_state->packet_open) {
        i2c_monitor_begin_packet(context);
    }

    if ((channel_state->packet_payload_offset + sizeof(i2c_decode_event_t)) > TRACE_PACKET_PAYLOAD_BYTES) {
        if (!i2c_monitor_flush_packet(context, false)) {
            return false;
        }
        i2c_monitor_begin_packet(context);
    }

    channel_state->packet.payload[channel_state->packet_payload_offset] = event_type;
    channel_state->packet.payload[channel_state->packet_payload_offset + 1u] = event_value;
    channel_state->packet_payload_offset += sizeof(i2c_decode_event_t);
    channel_state->packet_event_count += 1u;

    if (event_type == I2C_DECODE_EVENT_STOP) {
        return i2c_monitor_flush_packet(context, true);
    }

    return true;
}

static void i2c_monitor_dma_irq_handler(void) {
    uint32_t pending = dma_hw->ints0;

    for (uint32_t channel = 0u; channel < I2C_MONITOR_CHANNEL_COUNT; ++channel) {
        i2c_monitor_channel_state_t *channel_state = &g_i2c_monitor_channels[channel];
        uint32_t channel_mask;
        uint8_t completed_buffer;
        uint8_t next_buffer;
        i2c_monitor_emit_context_t emit_context;

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
        channel_state->completed_buffers += 1u;
        i2c_monitor_dma_restart(channel_state, next_buffer);

        if (!app_control_stream_enabled()) {
            i2c_monitor_reset_packet_state(channel_state);
            (void)i2c_decoder_process_buffer(
                &channel_state->decoder_state,
                channel_state->buffers[completed_buffer],
                I2C_MONITOR_BUFFER_WORDS,
                NULL,
                NULL
            );
            continue;
        }

        memset(&emit_context, 0, sizeof(emit_context));
        emit_context.channel_state = channel_state;

        if (!i2c_decoder_process_buffer(
                &channel_state->decoder_state,
                channel_state->buffers[completed_buffer],
                I2C_MONITOR_BUFFER_WORDS,
                i2c_monitor_emit_event,
                &emit_context
            )) {
            channel_state->overrun_count += 1u;
        }
    }
}

/**
 * @brief Configure GPIO, PIO, and DMA resources for one I2C sample channel.
 * @param channel_state Channel runtime state to initialize.
 */
static void i2c_monitor_init_channel(i2c_monitor_channel_state_t *channel_state) {
    dma_channel_config dma_config;
    pio_sm_config sm_config;
    float clkdiv;

    gpio_disable_pulls(channel_state->sda_gpio);
    gpio_disable_pulls(channel_state->scl_gpio);
    gpio_set_input_enabled(channel_state->sda_gpio, true);
    gpio_set_input_enabled(channel_state->scl_gpio, true);

    sm_config = i2c_monitor_sampler_program_get_default_config(g_i2c_monitor_program_offset);
    sm_config_set_in_pins(&sm_config, channel_state->sda_gpio);
    sm_config_set_in_shift(&sm_config, false, true, 32u);
    clkdiv = (float)clock_get_hz(clk_sys) / (float)channel_state->sample_hz;
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

    channel_state->dma_channel = dma_claim_unused_channel(true);
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
    dma_channel_set_irq0_enabled((uint)channel_state->dma_channel, true);

    memset(channel_state->buffers, 0, sizeof(channel_state->buffers));
    channel_state->active_buffer = 0u;
    channel_state->completed_buffers = 0u;
    channel_state->overrun_count = 0u;
    channel_state->emitted_packets = 0u;
    memset(&channel_state->packet, 0, sizeof(channel_state->packet));
    i2c_monitor_reset_packet_state(channel_state);
    i2c_decoder_init(&channel_state->decoder_state);
    i2c_monitor_dma_restart(channel_state, 0u);
}

/** @copydoc i2c_monitor_init */
void i2c_monitor_init(void) {
    if (g_i2c_monitor_initialized) {
        return;
    }

    g_i2c_monitor_program_offset = pio_add_program(i2c_monitor_pio, &i2c_monitor_sampler_program);

    irq_set_exclusive_handler(DMA_IRQ_0, i2c_monitor_dma_irq_handler);
    irq_set_enabled(DMA_IRQ_0, true);

    for (uint32_t channel = 0u; channel < I2C_MONITOR_CHANNEL_COUNT; ++channel) {
        i2c_monitor_init_channel(&g_i2c_monitor_channels[channel]);
    }

    g_i2c_monitor_initialized = true;
}

