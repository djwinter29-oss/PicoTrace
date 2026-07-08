/**
 * @file spi_monitor.c
 * @brief SPI capture scaffold with DMA-backed per-lane sampling.
 */

#include "trace/capture/spi_monitor.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "pico/time.h"

#include "app_control.h"
#include "config/spi_monitor_config.h"
#include "trace/trace_ring.h"

#include "spi_monitor.pio.h"

#define SPI_MONITOR_LANES_PER_BUS 2u
#define SPI_MONITOR_DMA_HALF_COUNT 2u
#define SPI_MONITOR_DMA_RING_WORDS (SPI_MONITOR_DMA_BUFFER_WORDS * SPI_MONITOR_DMA_HALF_COUNT)
#define SPI_MONITOR_DMA_RING_BYTES (SPI_MONITOR_DMA_RING_WORDS * sizeof(uint32_t))
#define SPI_MONITOR_SAMPLES_PER_WORD 8u
#define SPI_MONITOR_BITS_PER_SAMPLE 3u
#define SPI_MONITOR_NO_ACTIVE_SLOT 0xFFu
#define SPI_MONITOR_MULTI_ACTIVE_SLOT 0xFEu

#if defined(_MSC_VER)
#define SPI_MONITOR_DMA_ALIGN(bytes)
#else
#define SPI_MONITOR_DMA_ALIGN(bytes) __attribute__((aligned(bytes)))
#endif

/** @brief Packet-builder state for one logical SPI channel session. */
typedef struct {
    bool packet_open; /**< Indicates whether a fixed trace packet fragment is currently open. */
    bool transaction_fragmented; /**< Indicates whether the current transaction already emitted an earlier fragment. */
    uint8_t pending_flags; /**< Flags to apply to the next opened fragment. */
    uint16_t payload_offset; /**< Number of payload bytes already staged in the open fragment. */
    trace_packet_t packet; /**< Caller-owned fixed packet fragment under construction. */
} spi_monitor_packet_builder_t;

/** @brief Runtime state owned by one logical SPI channel in the current scaffold. */
typedef struct {
    bool running; /**< Indicates whether this logical channel is currently enabled. */
    spi_monitor_capture_t capture; /**< Active lane selection for this logical channel. */
    uint8_t spi_mode; /**< Active SPI mode `0` through `3`. */
    uint32_t timeout_us; /**< Active inter-byte timeout in microseconds. */
    uint32_t packets_emitted; /**< Number of emitted trace packet fragments in the current session. */
    uint32_t sink_overrun_count; /**< Number of fragments dropped because the shared trace ring rejected them. */
    uint32_t overrun_count; /**< Number of dropped completed fragments in the current session. */
    spi_monitor_packet_builder_t packet_builder; /**< Fixed-packet builder that owns the current SPI fragment. */
} spi_monitor_channel_state_t;

/** @brief Runtime state shared by all logical channels that sit on one observed SPI bus. */
typedef struct {
    bool running; /**< Indicates whether this observed SPI bus is currently enabled. */
    spi_monitor_capture_t capture; /**< Bus-wide lane selection applied to all sibling logical channels. */
    uint8_t spi_mode; /**< Bus-wide SPI mode applied to all sibling logical channels. */
    uint8_t channel_select_mask; /**< Bit mask of selected chip-select slots on this bus. */
    uint32_t timeout_us; /**< Bus-wide inter-byte timeout applied to all sibling logical channels. */
} spi_monitor_bus_state_t;

/** @brief Transaction decode state shared by all logical channels on one observed SPI bus. */
typedef struct {
    bool transaction_open; /**< Indicates whether one logical SPI transaction is currently open on this bus. */
    uint8_t active_slot; /**< Active chip-select slot that currently owns the open transaction. */
    uint8_t bit_count; /**< Number of bits accumulated into the current SPI byte. */
    uint8_t mosi_byte; /**< MOSI byte currently under construction. */
    uint8_t miso_byte; /**< MISO byte currently under construction. */
    uint32_t last_activity_timestamp_us; /**< Timestamp of the most recently processed buffer attributed to this transaction. */
} spi_monitor_bus_runtime_t;

/** @brief DMA-backed sampler state owned by one physical SPI data lane. */
typedef struct {
    uint32_t bus; /**< Observed SPI bus index that owns this lane. */
    uint32_t pin_base; /**< Contiguous sampled pin base containing `SCLK`, `MOSI`, and `MISO`. */
    uint8_t sample_bit_index; /**< Bit position inside one 3-bit PIO sample for this lane. */
    uint sm; /**< PIO state machine dedicated to this physical data lane. */
    int dma_channel; /**< Claimed DMA channel for this physical data lane. */
    bool dma_claimed; /**< Indicates whether @ref dma_channel is reserved for this lane. */
    bool running; /**< Indicates whether this lane sampler is currently active. */
    uint32_t write_offset_words; /**< Most recently observed DMA write offset within the ping-pong ring. */
    uint32_t last_transfer_count; /**< Most recently observed DMA transfer count snapshot. */
    uint32_t completed_halves; /**< Number of completed ping-pong half-buffers consumed locally. */
    uint32_t overrun_count; /**< Number of completed half-buffers overwritten before service. */
} spi_monitor_lane_state_t;

/** @brief PIO instance reserved for the SPI monitor scaffold. */
static const PIO spi_monitor_pio = pio1;
/** @brief Logical channel identifiers exported on the shared trace packet header. */
static const uint8_t g_spi_monitor_logical_channels[SPI_MONITOR_CHANNEL_COUNT] = {
    SPI_MONITOR_CH0_LOGICAL_CHANNEL,
    SPI_MONITOR_CH1_LOGICAL_CHANNEL,
    SPI_MONITOR_CH2_LOGICAL_CHANNEL,
    SPI_MONITOR_CH3_LOGICAL_CHANNEL,
    SPI_MONITOR_CH4_LOGICAL_CHANNEL,
    SPI_MONITOR_CH5_LOGICAL_CHANNEL,
};
/** @brief Session state for every logical SPI channel. */
static spi_monitor_channel_state_t g_spi_monitor_channels[SPI_MONITOR_CHANNEL_COUNT];
/** @brief Shared runtime state for each observed SPI bus. */
static spi_monitor_bus_state_t g_spi_monitor_buses[SPI_MONITOR_BUS_COUNT];
/** @brief Decode state that tracks the current logical SPI transaction on each observed bus. */
static spi_monitor_bus_runtime_t g_spi_monitor_bus_runtimes[SPI_MONITOR_BUS_COUNT];
/** @brief DMA-backed capture state for every physical SPI data lane. */
static spi_monitor_lane_state_t g_spi_monitor_lanes[SPI_MONITOR_LANE_COUNT] = {
    {.bus = 0u, .pin_base = SPI_MONITOR_SPI0_SCLK_GPIO, .sample_bit_index = 1u, .sm = 0u, .dma_channel = -1},
    {.bus = 0u, .pin_base = SPI_MONITOR_SPI0_SCLK_GPIO, .sample_bit_index = 2u, .sm = 1u, .dma_channel = -1},
    {.bus = 1u, .pin_base = SPI_MONITOR_SPI1_SCLK_GPIO, .sample_bit_index = 1u, .sm = 2u, .dma_channel = -1},
    {.bus = 1u, .pin_base = SPI_MONITOR_SPI1_SCLK_GPIO, .sample_bit_index = 2u, .sm = 3u, .dma_channel = -1},
};
/* ponytail: each bus currently runs two identical PIO samplers and drops the unused lane bit in
 * software; this doubles FIFO and DMA traffic per bus, which is acceptable for the current 4-lane
 * board mapping. If state-machine or DMA pressure becomes a problem, replace this with one
 * sampler per bus and software lane demultiplexing at the shared buffer boundary.
 */
/** @brief Ping-pong DMA storage for every physical SPI data lane. */
SPI_MONITOR_DMA_ALIGN(SPI_MONITOR_DMA_RING_BYTES)
static uint32_t g_spi_monitor_lane_dma_buffers[SPI_MONITOR_LANE_COUNT][SPI_MONITOR_DMA_HALF_COUNT][SPI_MONITOR_DMA_BUFFER_WORDS];
/** @brief Installed program offsets for SPI modes `0` through `3`. */
static uint32_t g_spi_monitor_program_offsets[4];
/** @brief Indicates whether shared SPI monitor setup completed successfully. */
static bool g_spi_monitor_initialized;
/** @brief Sticky failure flag preventing repeated partial initialization attempts. */
static bool g_spi_monitor_init_failed;

/** @brief Return a coarse producer-side timestamp for newly opened SPI packet fragments. */
static uint32_t spi_monitor_timestamp_us(void) {
    return time_us_32();
}

/** @brief Return whether the shared trace ring accepted one completed SPI packet fragment. */
static bool spi_monitor_push_trace_packet(const trace_packet_t *packet) {
    return trace_ring_push(packet);
}

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

/** @brief Return whether SPI capture work should currently produce trace output. */
static bool spi_monitor_stream_enabled(void) {
    return app_control_stream_enabled();
}

/** @brief Map one logical SPI channel index to its owning observed SPI bus index. */
static uint32_t spi_monitor_channel_to_bus(uint32_t channel) {
    return channel / SPI_MONITOR_CS_SLOTS_PER_BUS;
}

/** @brief Return the first logical SPI channel index owned by one observed SPI bus. */
static uint32_t spi_monitor_bus_first_channel(uint32_t bus) {
    return bus * SPI_MONITOR_CS_SLOTS_PER_BUS;
}

/** @brief Return the logical SPI channel index that owns one bus-local chip-select slot. */
static uint32_t spi_monitor_bus_slot_to_channel(uint32_t bus, uint8_t slot) {
    return spi_monitor_bus_first_channel(bus) + slot;
}

/** @brief Return the first physical data lane owned by one observed SPI bus. */
static uint32_t spi_monitor_bus_first_lane(uint32_t bus) {
    return bus * SPI_MONITOR_LANES_PER_BUS;
}

/** @brief Return whether one bus capture mode should enable a given bus-local lane index. */
static bool spi_monitor_capture_uses_lane(spi_monitor_capture_t capture, uint32_t bus_lane) {
    if (capture == SPI_MONITOR_CAPTURE_MOSI) {
        return bus_lane == 0u;
    }

    if (capture == SPI_MONITOR_CAPTURE_MOSI_MISO) {
        return bus_lane < SPI_MONITOR_LANES_PER_BUS;
    }

    return false;
}

/** @brief Return the log2 ring size exponent required by the DMA write-address wrapper. */
static uint32_t spi_monitor_dma_ring_bits(void) {
    uint32_t bits = 0u;
    uint32_t bytes = SPI_MONITOR_DMA_RING_BYTES;

    while (bytes > 1u) {
        bytes >>= 1u;
        bits += 1u;
    }

    return bits;
}

/** @brief Reset packet-builder state for one logical SPI channel. */
static void spi_monitor_packet_builder_reset(spi_monitor_packet_builder_t *builder) {
    builder->packet_open = false;
    builder->transaction_fragmented = false;
    builder->pending_flags = 0u;
    builder->payload_offset = 0u;
    memset(&builder->packet, 0, sizeof(builder->packet));
}

/** @brief Drop only the currently open SPI fragment while preserving transaction-level continuation state. */
static void spi_monitor_packet_builder_drop_open_packet(spi_monitor_packet_builder_t *builder) {
    builder->packet_open = false;
    builder->payload_offset = 0u;
    memset(&builder->packet, 0, sizeof(builder->packet));
}

/** @brief Reset the active transaction state for one observed SPI bus. */
static void spi_monitor_reset_bus_runtime(spi_monitor_bus_runtime_t *bus_runtime) {
    memset(bus_runtime, 0, sizeof(*bus_runtime));
    bus_runtime->active_slot = SPI_MONITOR_NO_ACTIVE_SLOT;
}

/** @brief Discard any open SPI packet builders owned by one observed bus. */
static void spi_monitor_discard_bus_packets(uint32_t bus) {
    uint32_t first_channel = spi_monitor_bus_first_channel(bus);

    for (uint32_t channel = first_channel; channel < (first_channel + SPI_MONITOR_CS_SLOTS_PER_BUS); ++channel) {
        spi_monitor_packet_builder_reset(&g_spi_monitor_channels[channel].packet_builder);
    }
}

/** @brief Abort one observed SPI bus transaction without flushing any partial fragment to the ring. */
static void spi_monitor_abort_bus_transaction(uint32_t bus) {
    spi_monitor_discard_bus_packets(bus);
    spi_monitor_reset_bus_runtime(&g_spi_monitor_bus_runtimes[bus]);
}

/** @brief Return the GPIO number for one observed chip-select slot. */
static uint32_t spi_monitor_bus_slot_gpio(uint32_t bus, uint8_t slot) {
    static const uint8_t g_spi_monitor_bus_cs_gpios[SPI_MONITOR_BUS_COUNT][SPI_MONITOR_CS_SLOTS_PER_BUS] = {
        {SPI_MONITOR_SPI0_CS0_GPIO, SPI_MONITOR_SPI0_CS1_GPIO, SPI_MONITOR_SPI0_CS2_GPIO},
        {SPI_MONITOR_SPI1_CS0_GPIO, SPI_MONITOR_SPI1_CS1_GPIO, SPI_MONITOR_SPI1_CS2_GPIO},
    };

    return g_spi_monitor_bus_cs_gpios[bus][slot];
}

/** @brief Sample which chip-select slots are currently asserted on one observed SPI bus. */
static uint8_t spi_monitor_sample_active_cs_mask(uint32_t bus) {
    uint8_t active_mask = 0u;

    for (uint8_t slot = 0u; slot < SPI_MONITOR_CS_SLOTS_PER_BUS; ++slot) {
        if (!gpio_get(spi_monitor_bus_slot_gpio(bus, slot))) {
            active_mask = (uint8_t)(active_mask | (uint8_t)(1u << slot));
        }
    }

    return active_mask;
}

/** @brief Return the selected bus-local chip-select slot after applying the configured selection mask. */
static uint8_t spi_monitor_select_active_slot(uint32_t bus, uint8_t active_cs_mask) {
    uint8_t eligible_mask = (uint8_t)(active_cs_mask & g_spi_monitor_buses[bus].channel_select_mask);

    if (eligible_mask == 0u) {
        return SPI_MONITOR_NO_ACTIVE_SLOT;
    }

    if ((eligible_mask & (uint8_t)(eligible_mask - 1u)) != 0u) {
        return SPI_MONITOR_MULTI_ACTIVE_SLOT;
    }

    for (uint8_t slot = 0u; slot < SPI_MONITOR_CS_SLOTS_PER_BUS; ++slot) {
        if ((eligible_mask & (uint8_t)(1u << slot)) != 0u) {
            return slot;
        }
    }

    return SPI_MONITOR_NO_ACTIVE_SLOT;
}

/** @brief Mark flags that should be applied to the next opened SPI packet fragment. */
static void spi_monitor_packet_builder_mark_next(spi_monitor_packet_builder_t *builder, uint8_t flags) {
    builder->pending_flags |= flags;
}

/** @brief Open a new fixed SPI trace packet fragment for one logical channel. */
static void spi_monitor_packet_builder_begin(
    spi_monitor_channel_state_t *channel_state,
    uint8_t logical_channel,
    uint32_t timestamp_us
) {
    spi_monitor_packet_builder_t *builder = &channel_state->packet_builder;

    builder->packet.header.version = TRACE_PACKET_VERSION;
    builder->packet.header.type = TRACE_TYPE_SPI;
    builder->packet.header.channel = logical_channel;
    builder->packet.header.flags = builder->pending_flags;
    builder->packet.header.payload_len = 0u;
    builder->packet.header.meta = (uint16_t)channel_state->capture;
    builder->packet.header.sequence = channel_state->packets_emitted + 1u;
    builder->packet.header.timestamp_us = timestamp_us;
    if (builder->transaction_fragmented) {
        builder->packet.header.flags |= TRACE_FLAG_CONTINUED;
    }
    builder->pending_flags = 0u;
    builder->payload_offset = 0u;
    builder->packet_open = true;
}

/** @brief Flush the currently open SPI packet fragment into the shared trace ring. */
static bool spi_monitor_packet_builder_flush(
    spi_monitor_channel_state_t *channel_state,
    uint8_t final_flags,
    bool end_of_transaction
) {
    spi_monitor_packet_builder_t *builder = &channel_state->packet_builder;

    if (!builder->packet_open) {
        builder->transaction_fragmented = false;
        return true;
    }

    builder->packet.header.flags = (uint8_t)(builder->packet.header.flags | final_flags);
    if (end_of_transaction) {
        builder->packet.header.flags |= TRACE_FLAG_END;
    }
    builder->packet.header.payload_len = (uint16_t)builder->payload_offset;
    if (!spi_monitor_push_trace_packet(&builder->packet)) {
        channel_state->sink_overrun_count += 1u;
        spi_monitor_packet_builder_drop_open_packet(builder);
        spi_monitor_packet_builder_mark_next(builder, TRACE_FLAG_OVERFLOW);
        return false;
    }

    channel_state->packets_emitted += 1u;
    builder->packet_open = false;
    builder->payload_offset = 0u;
    builder->transaction_fragmented = !end_of_transaction;
    memset(&builder->packet, 0, sizeof(builder->packet));
    return true;
}

/** @brief Append one SPI data byte pair to the current logical channel transaction. */
static bool spi_monitor_channel_append_byte_pair(
    spi_monitor_channel_state_t *channel_state,
    uint8_t logical_channel,
    uint32_t timestamp_us,
    uint8_t mosi_byte,
    uint8_t miso_byte
) {
    spi_monitor_packet_builder_t *builder = &channel_state->packet_builder;
    uint16_t bytes_needed = (channel_state->capture == SPI_MONITOR_CAPTURE_MOSI_MISO) ? 2u : 1u;

    if (!builder->packet_open) {
        spi_monitor_packet_builder_begin(channel_state, logical_channel, timestamp_us);
    }

    if ((builder->payload_offset + bytes_needed) > TRACE_PACKET_PAYLOAD_BYTES) {
        (void)spi_monitor_packet_builder_flush(channel_state, 0u, false);
        spi_monitor_packet_builder_begin(channel_state, logical_channel, timestamp_us);
    }

    builder->packet.payload[builder->payload_offset++] = mosi_byte;
    if (channel_state->capture == SPI_MONITOR_CAPTURE_MOSI_MISO) {
        builder->packet.payload[builder->payload_offset++] = miso_byte;
    }

    return true;
}

/** @brief Close the current logical transaction on one observed SPI bus. */
static void spi_monitor_close_bus_transaction(uint32_t bus, uint8_t closing_flags) {
    spi_monitor_bus_runtime_t *bus_runtime = &g_spi_monitor_bus_runtimes[bus];

    if (!bus_runtime->transaction_open) {
        spi_monitor_reset_bus_runtime(bus_runtime);
        return;
    }

    if (bus_runtime->active_slot < SPI_MONITOR_CS_SLOTS_PER_BUS) {
        uint32_t channel = spi_monitor_bus_slot_to_channel(bus, bus_runtime->active_slot);
        spi_monitor_channel_state_t *channel_state = &g_spi_monitor_channels[channel];

        if (bus_runtime->bit_count != 0u) {
            closing_flags |= TRACE_FLAG_TRUNCATED;
        }
        (void)spi_monitor_packet_builder_flush(channel_state, closing_flags, true);
    }

    spi_monitor_reset_bus_runtime(bus_runtime);
}

/** @brief Ensure that one observed SPI bus is attributed to the currently selected chip-select slot. */
static void spi_monitor_open_bus_transaction(uint32_t bus, uint8_t active_slot, uint32_t timestamp_us) {
    spi_monitor_bus_runtime_t *bus_runtime = &g_spi_monitor_bus_runtimes[bus];

    if (bus_runtime->transaction_open && (bus_runtime->active_slot == active_slot)) {
        bus_runtime->last_activity_timestamp_us = timestamp_us;
        return;
    }

    spi_monitor_close_bus_transaction(bus, 0u);
    bus_runtime->transaction_open = true;
    bus_runtime->active_slot = active_slot;
    bus_runtime->last_activity_timestamp_us = timestamp_us;
}

/** @brief Decode one staged DMA half-buffer for one observed bus using the routed lane buffers. */
static void spi_monitor_process_bus_half(
    uint32_t bus,
    uint32_t half_index,
    uint8_t active_cs_mask,
    uint32_t timestamp_us,
    uint32_t raw_word_count
) {
    spi_monitor_bus_runtime_t *bus_runtime = &g_spi_monitor_bus_runtimes[bus];
    uint32_t first_lane = spi_monitor_bus_first_lane(bus);
    const spi_monitor_lane_state_t *mosi_lane = &g_spi_monitor_lanes[first_lane];
    const spi_monitor_lane_state_t *miso_lane = &g_spi_monitor_lanes[first_lane + 1u];
    const uint32_t *mosi_raw_words = g_spi_monitor_lane_dma_buffers[first_lane][half_index];
    const uint32_t *miso_raw_words = g_spi_monitor_lane_dma_buffers[first_lane + 1u][half_index];
    uint8_t active_slot;

    if ((raw_word_count == 0u) || !g_spi_monitor_buses[bus].running) {
        return;
    }

    active_slot = spi_monitor_select_active_slot(bus, active_cs_mask);
    if (active_slot == SPI_MONITOR_MULTI_ACTIVE_SLOT) {
        spi_monitor_close_bus_transaction(bus, TRACE_FLAG_ERROR);
        return;
    }

    if (active_slot == SPI_MONITOR_NO_ACTIVE_SLOT) {
        spi_monitor_close_bus_transaction(bus, 0u);
        return;
    }

    spi_monitor_open_bus_transaction(bus, active_slot, timestamp_us);

    for (uint32_t word_index = 0u; word_index < raw_word_count; ++word_index) {
        uint32_t packed_mosi_samples = mosi_raw_words[word_index];
        uint32_t packed_miso_samples = miso_raw_words[word_index];

        for (uint32_t sample_in_word = 0u; sample_in_word < SPI_MONITOR_SAMPLES_PER_WORD; ++sample_in_word) {
            uint8_t mosi_sample = (uint8_t)((packed_mosi_samples >> 21u) & 0x07u);
            uint8_t miso_sample = (uint8_t)((packed_miso_samples >> 21u) & 0x07u);
            uint8_t mosi_bit = (uint8_t)((mosi_sample >> mosi_lane->sample_bit_index) & 0x01u);
            uint8_t miso_bit = 0u;
            uint32_t channel = spi_monitor_bus_slot_to_channel(bus, bus_runtime->active_slot);
            spi_monitor_channel_state_t *channel_state = &g_spi_monitor_channels[channel];

            if (channel_state->capture == SPI_MONITOR_CAPTURE_MOSI_MISO) {
                miso_bit = (uint8_t)((miso_sample >> miso_lane->sample_bit_index) & 0x01u);
            }

            packed_mosi_samples <<= SPI_MONITOR_BITS_PER_SAMPLE;
            packed_miso_samples <<= SPI_MONITOR_BITS_PER_SAMPLE;
            bus_runtime->mosi_byte = (uint8_t)((bus_runtime->mosi_byte << 1u) | mosi_bit);
            bus_runtime->miso_byte = (uint8_t)((bus_runtime->miso_byte << 1u) | miso_bit);
            bus_runtime->bit_count += 1u;
            if (bus_runtime->bit_count != 8u) {
                continue;
            }

            (void)spi_monitor_channel_append_byte_pair(
                channel_state,
                g_spi_monitor_logical_channels[channel],
                timestamp_us,
                bus_runtime->mosi_byte,
                bus_runtime->miso_byte
            );
            bus_runtime->bit_count = 0u;
            bus_runtime->mosi_byte = 0u;
            bus_runtime->miso_byte = 0u;
        }
    }

    bus_runtime->last_activity_timestamp_us = timestamp_us;
}

/** @brief Decode one authoritative raw SPI buffer for one observed bus and emit fixed trace packets. */
static void spi_monitor_process_bus_buffer(
    uint32_t bus,
    uint8_t active_cs_mask,
    uint32_t timestamp_us,
    const uint32_t *raw_words,
    uint32_t raw_word_count
) {
    if ((raw_words == NULL) || (raw_word_count == 0u) || !g_spi_monitor_buses[bus].running) {
        return;
    }

    memcpy(g_spi_monitor_lane_dma_buffers[spi_monitor_bus_first_lane(bus)][0], raw_words, raw_word_count * sizeof(raw_words[0]));
    memcpy(g_spi_monitor_lane_dma_buffers[spi_monitor_bus_first_lane(bus) + 1u][0], raw_words, raw_word_count * sizeof(raw_words[0]));
    spi_monitor_process_bus_half(bus, 0u, active_cs_mask, timestamp_us, raw_word_count);
}

/** @brief Close timed-out SPI transactions whose chip-select did not explicitly end in time. */
static void spi_monitor_poll_bus_timeout(uint32_t bus, uint32_t now_us) {
    spi_monitor_bus_runtime_t *bus_runtime = &g_spi_monitor_bus_runtimes[bus];
    uint32_t timeout_us = g_spi_monitor_buses[bus].timeout_us;

    if (!bus_runtime->transaction_open || (timeout_us == 0u)) {
        return;
    }

    if ((now_us - bus_runtime->last_activity_timestamp_us) >= timeout_us) {
        spi_monitor_close_bus_transaction(bus, 0u);
    }
}

/** @brief Clear volatile DMA-owned state for one physical SPI data lane. */
static void spi_monitor_reset_lane_state(spi_monitor_lane_state_t *lane_state) {
    lane_state->running = false;
    lane_state->write_offset_words = 0u;
    lane_state->last_transfer_count = 0u;
    lane_state->completed_halves = 0u;
    lane_state->overrun_count = 0u;
}

/** @brief Drop DMA progress accumulated while SPI trace output is disabled. */
static void spi_monitor_discard_lane_backlog(uint32_t lane) {
    spi_monitor_lane_state_t *lane_state = &g_spi_monitor_lanes[lane];
    uint32_t transfer_count;
    uint32_t words_written;

    if (!lane_state->running) {
        return;
    }

    transfer_count = dma_channel_hw_addr((uint)lane_state->dma_channel)->transfer_count;
    words_written = lane_state->last_transfer_count - transfer_count;
    lane_state->last_transfer_count = transfer_count;
    lane_state->write_offset_words = (lane_state->write_offset_words + (words_written % SPI_MONITOR_DMA_RING_WORDS)) % SPI_MONITOR_DMA_RING_WORDS;
}

/** @brief Return the aggregate lane overrun count for one observed SPI bus. */
static uint32_t spi_monitor_bus_lane_overrun_count(uint32_t bus) {
    uint32_t first_lane = spi_monitor_bus_first_lane(bus);
    uint32_t overrun_count = 0u;

    for (uint32_t bus_lane = 0u; bus_lane < SPI_MONITOR_LANES_PER_BUS; ++bus_lane) {
        const spi_monitor_lane_state_t *lane_state = &g_spi_monitor_lanes[first_lane + bus_lane];

        if (lane_state->running) {
            overrun_count += lane_state->overrun_count;
        }
    }

    return overrun_count;
}

/** @brief Refresh public per-channel counters from the active bus-owned DMA lanes. */
static void spi_monitor_refresh_channel_counters(uint32_t bus) {
    uint32_t first_channel = spi_monitor_bus_first_channel(bus);
    uint32_t lane_overrun_count = spi_monitor_bus_lane_overrun_count(bus);

    for (uint32_t channel = first_channel; channel < (first_channel + SPI_MONITOR_CS_SLOTS_PER_BUS); ++channel) {
        if (g_spi_monitor_channels[channel].running) {
            g_spi_monitor_channels[channel].overrun_count =
                g_spi_monitor_channels[channel].sink_overrun_count + lane_overrun_count;
        }
    }
}

/** @brief Stop one physical SPI data lane and leave its DMA ring idle. */
static void spi_monitor_stop_lane(uint32_t lane) {
    spi_monitor_lane_state_t *lane_state = &g_spi_monitor_lanes[lane];

    if (lane_state->dma_claimed) {
        dma_channel_abort((uint)lane_state->dma_channel);
    }

    pio_sm_set_enabled(spi_monitor_pio, lane_state->sm, false);
    pio_sm_clear_fifos(spi_monitor_pio, lane_state->sm);
    spi_monitor_reset_lane_state(lane_state);
}

/** @brief Start one physical SPI data lane with a continuous DMA ping-pong ring. */
static bool spi_monitor_start_lane(uint32_t lane, uint8_t spi_mode) {
    spi_monitor_lane_state_t *lane_state = &g_spi_monitor_lanes[lane];
    dma_channel_config dma_config;

    memset(g_spi_monitor_lane_dma_buffers[lane], 0, sizeof(g_spi_monitor_lane_dma_buffers[lane]));

    switch (spi_mode) {
        case 0u:
            spi_monitor_mode0_sampler_program_init(spi_monitor_pio, lane_state->sm, g_spi_monitor_program_offsets[0], lane_state->pin_base, 1.0f);
            break;
        case 1u:
            spi_monitor_mode1_sampler_program_init(spi_monitor_pio, lane_state->sm, g_spi_monitor_program_offsets[1], lane_state->pin_base, 1.0f);
            break;
        case 2u:
            spi_monitor_mode2_sampler_program_init(spi_monitor_pio, lane_state->sm, g_spi_monitor_program_offsets[2], lane_state->pin_base, 1.0f);
            break;
        default:
            spi_monitor_mode3_sampler_program_init(spi_monitor_pio, lane_state->sm, g_spi_monitor_program_offsets[3], lane_state->pin_base, 1.0f);
            break;
    }

    dma_config = dma_channel_get_default_config((uint)lane_state->dma_channel);
    channel_config_set_transfer_data_size(&dma_config, DMA_SIZE_32);
    channel_config_set_read_increment(&dma_config, false);
    channel_config_set_write_increment(&dma_config, true);
    channel_config_set_dreq(&dma_config, pio_get_dreq(spi_monitor_pio, lane_state->sm, false));
    channel_config_set_ring(&dma_config, true, spi_monitor_dma_ring_bits());
    dma_channel_configure(
        (uint)lane_state->dma_channel,
        &dma_config,
        &g_spi_monitor_lane_dma_buffers[lane][0][0],
        &spi_monitor_pio->rxf[lane_state->sm],
        UINT32_MAX,
        true
    );

    lane_state->running = true;
    lane_state->write_offset_words = 0u;
    lane_state->last_transfer_count = UINT32_MAX;
    lane_state->completed_halves = 0u;
    lane_state->overrun_count = 0u;
    return true;
}

/** @brief Start or stop the physical SPI data lanes that belong to one observed bus. */
static bool spi_monitor_apply_bus_lane_capture(uint32_t bus, spi_monitor_capture_t capture, uint8_t spi_mode) {
    uint32_t first_lane = spi_monitor_bus_first_lane(bus);

    for (uint32_t bus_lane = 0u; bus_lane < SPI_MONITOR_LANES_PER_BUS; ++bus_lane) {
        uint32_t lane = first_lane + bus_lane;

        spi_monitor_stop_lane(lane);
        if (spi_monitor_capture_uses_lane(capture, bus_lane) && !spi_monitor_start_lane(lane, spi_mode)) {
            for (uint32_t rollback = 0u; rollback <= bus_lane; ++rollback) {
                spi_monitor_stop_lane(first_lane + rollback);
            }
            return false;
        }
    }

    return true;
}

/** @brief Consume one completed DMA half-buffer locally until CS-aware packetization lands. */
static void spi_monitor_consume_lane_half(uint32_t lane, uint32_t half_index) {
    spi_monitor_lane_state_t *lane_state = &g_spi_monitor_lanes[lane];

    if ((lane % SPI_MONITOR_LANES_PER_BUS) == 0u) {
        /* ponytail: CS_N is sampled once per completed DMA half-buffer instead of per raw SPI bit.
         * That keeps the current 3-pin PIO sampler and 4-lane DMA layout intact, and it is
         * acceptable for the current monitor model because a controller usually leaves a clock gap
         * when CS_N changes and starts the next transaction after that boundary. If later hardware
         * proves to switch CS_N mid-burst without a usable clock gap, upgrade the raw sampler to
         * carry CS bits in the DMA stream and decode those historical CS transitions directly.
         */
        spi_monitor_process_bus_half(
            lane_state->bus,
            half_index,
            spi_monitor_sample_active_cs_mask(lane_state->bus),
            spi_monitor_timestamp_us(),
            SPI_MONITOR_DMA_BUFFER_WORDS
        );
    }

    lane_state->completed_halves += 1u;
    spi_monitor_refresh_channel_counters(lane_state->bus);
}

/** @brief Mark one half-buffer complete for one physical SPI data lane. */
static void spi_monitor_mark_lane_half_complete(uint32_t lane, uint32_t half_index) {
    spi_monitor_consume_lane_half(lane, half_index);
}

/** @brief Service DMA progress for one running physical SPI data lane. */
static void spi_monitor_poll_lane(uint32_t lane) {
    spi_monitor_lane_state_t *lane_state = &g_spi_monitor_lanes[lane];
    uint32_t transfer_count;
    uint32_t words_written;
    uint32_t offset;

    if (!lane_state->running) {
        return;
    }

    transfer_count = dma_channel_hw_addr((uint)lane_state->dma_channel)->transfer_count;
    words_written = lane_state->last_transfer_count - transfer_count;
    if (words_written == 0u) {
        return;
    }

    lane_state->last_transfer_count = transfer_count;
    if (words_written >= SPI_MONITOR_DMA_RING_WORDS) {
        lane_state->overrun_count += (words_written / SPI_MONITOR_DMA_RING_WORDS);
        words_written %= SPI_MONITOR_DMA_RING_WORDS;
    }
    offset = lane_state->write_offset_words;
    while (words_written != 0u) {
        uint32_t words_until_boundary = SPI_MONITOR_DMA_BUFFER_WORDS - (offset % SPI_MONITOR_DMA_BUFFER_WORDS);

        if (words_written < words_until_boundary) {
            offset = (offset + words_written) % SPI_MONITOR_DMA_RING_WORDS;
            words_written = 0u;
            break;
        }

        words_written -= words_until_boundary;
        spi_monitor_mark_lane_half_complete(lane, offset / SPI_MONITOR_DMA_BUFFER_WORDS);
        offset = (offset + words_until_boundary) % SPI_MONITOR_DMA_RING_WORDS;
    }

    lane_state->write_offset_words = offset;
    spi_monitor_refresh_channel_counters(lane_state->bus);
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
    channel_state->sink_overrun_count = 0u;
    channel_state->overrun_count = 0u;
    spi_monitor_packet_builder_reset(&channel_state->packet_builder);
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
    uint32_t packets_emitted = 0u;
    uint32_t sink_overrun_count = 0u;

    for (uint32_t channel = spi_monitor_bus_first_channel(bus);
         channel < (spi_monitor_bus_first_channel(bus) + SPI_MONITOR_CS_SLOTS_PER_BUS);
         ++channel) {
        packets_emitted += g_spi_monitor_channels[channel].packets_emitted;
        sink_overrun_count += g_spi_monitor_channels[channel].sink_overrun_count;
    }

    memset(status_out, 0, sizeof(*status_out));
    status_out->initialized = g_spi_monitor_initialized && !g_spi_monitor_init_failed;
    status_out->running = bus_state->running;
    status_out->capture = bus_state->capture;
    status_out->spi_mode = bus_state->spi_mode;
    status_out->channel_select_mask = bus_state->channel_select_mask;
    status_out->timeout_us = bus_state->timeout_us;
    status_out->packets_emitted = packets_emitted;
    status_out->overrun_count = sink_overrun_count + spi_monitor_bus_lane_overrun_count(bus);
}

/** @copydoc spi_monitor_init */
spi_monitor_rc_t spi_monitor_init(void) {
    uint32_t lane;

    if (g_spi_monitor_initialized) {
        return SPI_MONITOR_RC_OK;
    }

    if (g_spi_monitor_init_failed) {
        return SPI_MONITOR_RC_FAILED;
    }

    memset(g_spi_monitor_channels, 0, sizeof(g_spi_monitor_channels));
    memset(g_spi_monitor_buses, 0, sizeof(g_spi_monitor_buses));
    memset(g_spi_monitor_bus_runtimes, 0, sizeof(g_spi_monitor_bus_runtimes));
    for (lane = 0u; lane < SPI_MONITOR_LANE_COUNT; ++lane) {
        spi_monitor_reset_lane_state(&g_spi_monitor_lanes[lane]);
        g_spi_monitor_lanes[lane].dma_channel = dma_claim_unused_channel(false);
        if (g_spi_monitor_lanes[lane].dma_channel < 0) {
            g_spi_monitor_init_failed = true;
            for (uint32_t claimed = 0u; claimed < lane; ++claimed) {
                if (g_spi_monitor_lanes[claimed].dma_claimed) {
                    dma_channel_unclaim((uint)g_spi_monitor_lanes[claimed].dma_channel);
                    g_spi_monitor_lanes[claimed].dma_claimed = false;
                    g_spi_monitor_lanes[claimed].dma_channel = -1;
                }
            }
            return SPI_MONITOR_RC_FAILED;
        }
        g_spi_monitor_lanes[lane].dma_claimed = true;
    }
    g_spi_monitor_program_offsets[0] = pio_add_program(spi_monitor_pio, &spi_monitor_mode0_sampler_program);
    g_spi_monitor_program_offsets[1] = pio_add_program(spi_monitor_pio, &spi_monitor_mode1_sampler_program);
    g_spi_monitor_program_offsets[2] = pio_add_program(spi_monitor_pio, &spi_monitor_mode2_sampler_program);
    g_spi_monitor_program_offsets[3] = pio_add_program(spi_monitor_pio, &spi_monitor_mode3_sampler_program);
    g_spi_monitor_initialized = true;
    return SPI_MONITOR_RC_OK;
}

/** @copydoc spi_monitor_poll */
void spi_monitor_poll(void) {
    uint32_t now_us = spi_monitor_timestamp_us();
    bool stream_enabled = spi_monitor_stream_enabled();

    if (!stream_enabled) {
        for (uint32_t bus = 0u; bus < SPI_MONITOR_BUS_COUNT; ++bus) {
            spi_monitor_abort_bus_transaction(bus);
            spi_monitor_refresh_channel_counters(bus);
        }
        for (uint32_t lane = 0u; lane < SPI_MONITOR_LANE_COUNT; ++lane) {
            spi_monitor_discard_lane_backlog(lane);
        }
        return;
    }

    for (uint32_t lane = 0u; lane < SPI_MONITOR_LANE_COUNT; ++lane) {
        spi_monitor_poll_lane(lane);
    }

    if (stream_enabled) {
        for (uint32_t bus = 0u; bus < SPI_MONITOR_BUS_COUNT; ++bus) {
            spi_monitor_poll_bus_timeout(bus, now_us);
            spi_monitor_refresh_channel_counters(bus);
        }
    }
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

    if ((config->capture != SPI_MONITOR_CAPTURE_DISABLED) && !spi_monitor_stream_enabled()) {
        return SPI_MONITOR_RC_DISABLED;
    }

    bus_state = &g_spi_monitor_buses[bus];
    first_channel = spi_monitor_bus_first_channel(bus);
    if (config->capture == SPI_MONITOR_CAPTURE_DISABLED) {
        spi_monitor_close_bus_transaction(bus, 0u);
        spi_monitor_apply_bus_lane_capture(bus, SPI_MONITOR_CAPTURE_DISABLED, 0u);
        for (channel = first_channel; channel < (first_channel + SPI_MONITOR_CS_SLOTS_PER_BUS); ++channel) {
            spi_monitor_reset_channel_state(&g_spi_monitor_channels[channel]);
        }
        spi_monitor_reset_bus_state(bus_state);
        spi_monitor_reset_bus_runtime(&g_spi_monitor_bus_runtimes[bus]);
        return SPI_MONITOR_RC_OK;
    }

    if (!spi_monitor_valid_channel_select_mask(config->channel_select_mask)) {
        return SPI_MONITOR_RC_INVALID;
    }

    spi_monitor_close_bus_transaction(bus, 0u);
    timeout_us = (config->timeout_us != 0u) ? config->timeout_us : SPI_MONITOR_TIMEOUT_US_DEFAULT;
    if (!spi_monitor_apply_bus_lane_capture(bus, config->capture, config->spi_mode)) {
        return SPI_MONITOR_RC_FAILED;
    }
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
        channel_state->sink_overrun_count = 0u;
        channel_state->overrun_count = 0u;
        spi_monitor_packet_builder_reset(&channel_state->packet_builder);
    }
    spi_monitor_reset_bus_runtime(&g_spi_monitor_bus_runtimes[bus]);
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

#if defined(SPI_MONITOR_TEST_HOOKS)
bool spi_monitor_test_feed_samples(
    uint32_t bus,
    uint8_t active_cs_mask,
    uint32_t timestamp_us,
    const uint32_t *raw_words,
    uint32_t raw_word_count
) {
    if (!spi_monitor_valid_bus(bus) || (raw_words == NULL) || (raw_word_count == 0u)) {
        return false;
    }

    if (!spi_monitor_stream_enabled()) {
        spi_monitor_abort_bus_transaction(bus);
        spi_monitor_refresh_channel_counters(bus);
        return true;
    }

    spi_monitor_process_bus_buffer(bus, active_cs_mask, timestamp_us, raw_words, raw_word_count);
    spi_monitor_refresh_channel_counters(bus);
    return true;
}

bool spi_monitor_test_feed_dual_lane_samples(
    uint32_t bus,
    uint8_t active_cs_mask,
    uint32_t timestamp_us,
    const uint32_t *mosi_raw_words,
    const uint32_t *miso_raw_words,
    uint32_t raw_word_count
) {
    uint32_t first_lane;

    if (!spi_monitor_valid_bus(bus)
            || (mosi_raw_words == NULL)
            || (miso_raw_words == NULL)
            || (raw_word_count == 0u)
            || (raw_word_count > SPI_MONITOR_DMA_BUFFER_WORDS)) {
        return false;
    }

    if (!spi_monitor_stream_enabled()) {
        spi_monitor_abort_bus_transaction(bus);
        spi_monitor_refresh_channel_counters(bus);
        return true;
    }

    first_lane = spi_monitor_bus_first_lane(bus);
    memset(g_spi_monitor_lane_dma_buffers[first_lane][0], 0, sizeof(g_spi_monitor_lane_dma_buffers[first_lane][0]));
    memset(g_spi_monitor_lane_dma_buffers[first_lane + 1u][0], 0, sizeof(g_spi_monitor_lane_dma_buffers[first_lane + 1u][0]));
    memcpy(g_spi_monitor_lane_dma_buffers[first_lane][0], mosi_raw_words, raw_word_count * sizeof(mosi_raw_words[0]));
    memcpy(g_spi_monitor_lane_dma_buffers[first_lane + 1u][0], miso_raw_words, raw_word_count * sizeof(miso_raw_words[0]));
    spi_monitor_process_bus_half(bus, 0u, active_cs_mask, timestamp_us, raw_word_count);
    spi_monitor_refresh_channel_counters(bus);
    return true;
}

void spi_monitor_test_poll_timeout(uint32_t bus, uint32_t timestamp_us) {
    if (!spi_monitor_valid_bus(bus)) {
        return;
    }

    if (!spi_monitor_stream_enabled()) {
        spi_monitor_abort_bus_transaction(bus);
        spi_monitor_refresh_channel_counters(bus);
        return;
    }

    spi_monitor_poll_bus_timeout(bus, timestamp_us);
    spi_monitor_refresh_channel_counters(bus);
}

/**
 * @brief Inject synthetic DMA overrun counters for both physical data lanes on one observed bus.
 * @param bus Zero-based observed SPI bus index.
 * @param mosi_overruns Completed-half overruns to assign to the MOSI lane.
 * @param miso_overruns Completed-half overruns to assign to the MISO lane.
 */
void spi_monitor_test_set_lane_overrun_counts(uint32_t bus, uint32_t mosi_overruns, uint32_t miso_overruns) {
    uint32_t first_lane;

    if (!spi_monitor_valid_bus(bus)) {
        return;
    }

    first_lane = spi_monitor_bus_first_lane(bus);
    g_spi_monitor_lanes[first_lane].overrun_count = mosi_overruns;
    g_spi_monitor_lanes[first_lane + 1u].overrun_count = miso_overruns;
    spi_monitor_refresh_channel_counters(bus);
}
#endif