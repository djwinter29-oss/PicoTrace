/**
 * @file spi_monitor.c
 * @brief SPI capture scaffold with DMA-backed per-bus sampling.
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

#define SPI_MONITOR_PROGRAM_MODE_COUNT 4u
#define SPI_MONITOR_PIO_PROGRAM_WORD_COUNT 4u
#define SPI_MONITOR_DMA_HALF_COUNT 2u
#define SPI_MONITOR_DMA_RING_WORDS (SPI_MONITOR_DMA_BUFFER_WORDS * SPI_MONITOR_DMA_HALF_COUNT)
#define SPI_MONITOR_DMA_RING_BYTES (SPI_MONITOR_DMA_RING_WORDS * sizeof(uint32_t))
#define SPI_MONITOR_DMA_RING_BITS 8u
#define SPI_MONITOR_SAMPLES_PER_WORD 8u
#define SPI_MONITOR_NO_ACTIVE_SLOT 0xFFu
#define SPI_MONITOR_MULTI_ACTIVE_SLOT 0xFEu

typedef char spi_monitor_dma_ring_size_must_match[
    (SPI_MONITOR_DMA_RING_BYTES == (1u << SPI_MONITOR_DMA_RING_BITS)) ? 1 : -1
];
typedef char spi_monitor_pio_programs_must_fit[
    (SPI_MONITOR_BUS_SAMPLER_COUNT * SPI_MONITOR_PROGRAM_MODE_COUNT * SPI_MONITOR_PIO_PROGRAM_WORD_COUNT <= 32u) ? 1 : -1
];

#if defined(_MSC_VER)
#define SPI_MONITOR_DMA_ALIGN(bytes)
#else
#define SPI_MONITOR_DMA_ALIGN(bytes) __attribute__((aligned(bytes)))
#endif

/** @brief Packet-builder state for the active bus-owned SPI transaction. */
typedef struct {
    bool packet_open; /**< Indicates whether a fixed trace packet fragment is currently open. */
    bool transaction_fragmented; /**< Indicates whether the current transaction already emitted an earlier fragment. */
    uint32_t transaction_sequence; /**< Sequence number assigned to the current logical SPI transaction. */
    uint8_t pending_flags; /**< Flags to apply to the next opened fragment. */
    uint16_t payload_offset; /**< Number of payload bytes already staged in the open fragment. */
    trace_packet_t packet; /**< Caller-owned fixed packet fragment under construction. */
} spi_monitor_packet_builder_t;

/** @brief Per-logical-channel accounting state retained across bus-owned transactions. */
typedef struct {
    uint32_t packets_emitted; /**< Number of emitted trace packet fragments in the current session. */
    uint32_t transactions_emitted; /**< Number of emitted logical SPI transactions in the current session. */
    uint32_t sink_overrun_count; /**< Number of fragments dropped because the shared trace ring rejected them. */
} spi_monitor_channel_state_t;

/** @brief Runtime state shared by all logical channels that sit on one observed SPI bus. */
typedef struct {
    bool running; /**< Indicates whether this observed SPI bus is currently enabled. */
    spi_monitor_capture_t capture; /**< Bus-wide capture direction applied to all sibling logical channels. */
    uint8_t spi_mode; /**< Bus-wide SPI mode applied to all sibling logical channels. */
    uint8_t channel_select_mask; /**< Bit mask of selected chip-select slots on this bus. */
    uint32_t timeout_us; /**< Bus-wide inter-byte timeout applied to all sibling logical channels. */
} spi_monitor_bus_state_t;

/** @brief Transaction decode state shared by all logical channels on one observed SPI bus. */
typedef struct {
    bool transaction_open; /**< Indicates whether one logical SPI transaction is currently open on this bus. */
    uint8_t active_slot; /**< Active chip-select slot that currently owns the open transaction. */
    uint32_t transaction_timestamp_us; /**< Timestamp captured when the current logical transaction opened. */
    uint32_t last_activity_timestamp_us; /**< Timestamp of the most recently processed buffer attributed to this transaction. */
    spi_monitor_packet_builder_t packet_builder; /**< Fixed-packet builder that owns the active bus transaction fragment. */
} spi_monitor_bus_runtime_t;

/** @brief DMA-backed sampler state owned by one observed SPI bus. */
typedef struct {
    uint32_t bus; /**< Observed SPI bus index that owns this sampler. */
    uint32_t pin_base; /**< Contiguous sampled pin base containing `SCLK`, `MOSI`, and `MISO`. */
    uint sm; /**< PIO state machine dedicated to this raw bus sampler. */
    int dma_channel; /**< Claimed DMA channel for this raw bus sampler. */
    bool dma_claimed; /**< Indicates whether @ref dma_channel is reserved for this sampler. */
    bool running; /**< Indicates whether this raw bus sampler is currently active. */
    uint32_t write_offset_words; /**< Most recently observed DMA write offset within the ping-pong ring. */
    uint32_t last_transfer_count; /**< Most recently observed DMA transfer count snapshot. */
    uint32_t overrun_count; /**< Number of completed half-buffers overwritten before service. */
} spi_monitor_bus_sampler_state_t;

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
/** @brief DMA-backed capture state for every observed SPI bus sampler. */
static spi_monitor_bus_sampler_state_t g_spi_monitor_bus_samplers[SPI_MONITOR_BUS_SAMPLER_COUNT] = {
    {.bus = 0u, .pin_base = SPI_MONITOR_SPI0_SCLK_GPIO, .sm = 0u, .dma_channel = -1},
    {.bus = 1u, .pin_base = SPI_MONITOR_SPI1_SCLK_GPIO, .sm = 1u, .dma_channel = -1},
};
/* ponytail: the current sampler stores one raw MOSI/MISO bit-pair stream per observed bus and
 * still attributes CS_N only at DMA half-buffer handoff; that keeps the implementation small and
 * is acceptable at the current trace rates. If exact mid-buffer CS_N attribution becomes a real
 * requirement, the upgrade path is a raw format that carries historical CS samples too.
 */
/** @brief Ping-pong DMA storage for every observed SPI bus sampler. */
SPI_MONITOR_DMA_ALIGN(SPI_MONITOR_DMA_RING_BYTES)
static uint32_t g_spi_monitor_bus_sampler_dma_buffers[SPI_MONITOR_BUS_SAMPLER_COUNT][SPI_MONITOR_DMA_HALF_COUNT][SPI_MONITOR_DMA_BUFFER_WORDS];
/** @brief Mutable instruction storage for each bus-local SPI sampler program copy. */
static uint16_t g_spi_monitor_program_instructions[SPI_MONITOR_BUS_SAMPLER_COUNT][SPI_MONITOR_PROGRAM_MODE_COUNT][SPI_MONITOR_PIO_PROGRAM_WORD_COUNT];
/** @brief Bus-local SPI sampler program copies patched to wait on the correct observed SCLK GPIO. */
static pio_program_t g_spi_monitor_programs[SPI_MONITOR_BUS_SAMPLER_COUNT][SPI_MONITOR_PROGRAM_MODE_COUNT];
/** @brief Installed program offsets for each bus-local SPI sampler mode. */
static uint32_t g_spi_monitor_program_offsets[SPI_MONITOR_BUS_SAMPLER_COUNT][SPI_MONITOR_PROGRAM_MODE_COUNT];
/** @brief Indicates whether shared SPI monitor setup completed successfully. */
static bool g_spi_monitor_initialized;
/** @brief Sticky failure flag preventing repeated partial initialization attempts. */
static bool g_spi_monitor_init_failed;
/** @brief Bit mask of observed SPI buses that currently need producer-core polling. */
static uint8_t g_spi_monitor_poll_bus_mask;

/** @brief Return a coarse producer-side timestamp for newly opened SPI packet fragments. */
static uint32_t spi_monitor_timestamp_us(void) {
    return time_us_32();
}

static void spi_monitor_set_poll_interest(uint32_t bus, bool poll_needed) {
    uint8_t bus_mask = (uint8_t)(1u << bus);

    if (poll_needed) {
        g_spi_monitor_poll_bus_mask = (uint8_t)(g_spi_monitor_poll_bus_mask | bus_mask);
        return;
    }

    g_spi_monitor_poll_bus_mask = (uint8_t)(g_spi_monitor_poll_bus_mask & (uint8_t)~bus_mask);
}

/** @brief Return whether the shared trace ring accepted one completed SPI packet fragment. */
static bool spi_monitor_push_trace_packet(const trace_packet_t *packet) {
    return trace_ring_push(packet);
}

/** @brief Return whether an observed SPI bus index is within the SPI monitor range. */
bool spi_monitor_internal_valid_bus(uint32_t bus) {
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
bool spi_monitor_internal_stream_enabled(void) {
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

/** @brief Return the inclusive-exclusive logical channel range owned by one observed SPI bus. */
static void spi_monitor_bus_channel_range(uint32_t bus, uint32_t *first_channel, uint32_t *end_channel) {
    uint32_t first = spi_monitor_bus_first_channel(bus);

    *first_channel = first;
    *end_channel = first + SPI_MONITOR_CS_SLOTS_PER_BUS;
}

/** @brief Return whether one logical SPI channel is currently selected by its owning bus config. */
static bool spi_monitor_channel_running(uint32_t channel) {
    uint32_t bus = spi_monitor_channel_to_bus(channel);
    uint32_t first_channel = spi_monitor_bus_first_channel(bus);
    uint8_t channel_mask = (uint8_t)(1u << (channel - first_channel));

    return g_spi_monitor_buses[bus].running && ((g_spi_monitor_buses[bus].channel_select_mask & channel_mask) != 0u);
}

/** @brief Clear the shared bus state back to the stopped session state. */
static void spi_monitor_reset_bus_state(spi_monitor_bus_state_t *bus_state);

/** @brief Clear one logical SPI channel back to the stopped session state. */
static void spi_monitor_reset_channel_state(spi_monitor_channel_state_t *channel_state);

/** @brief Reset packet-builder state for one logical SPI channel. */
static void spi_monitor_packet_builder_reset(spi_monitor_packet_builder_t *builder) {
    builder->packet_open = false;
    builder->transaction_fragmented = false;
    builder->transaction_sequence = 0u;
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
    spi_monitor_packet_builder_reset(&g_spi_monitor_bus_runtimes[bus].packet_builder);
}

/** @brief Abort one observed SPI bus transaction without flushing any partial fragment to the ring. */
void spi_monitor_internal_abort_bus_transaction(uint32_t bus) {
    spi_monitor_discard_bus_packets(bus);
    spi_monitor_reset_bus_runtime(&g_spi_monitor_bus_runtimes[bus]);
}

/** @brief Clear all session counters for the logical channels owned by one observed SPI bus. */
static void spi_monitor_reset_bus_channels(uint32_t bus) {
    uint32_t first_channel;
    uint32_t end_channel;

    spi_monitor_bus_channel_range(bus, &first_channel, &end_channel);
    for (uint32_t channel = first_channel; channel < end_channel; ++channel) {
        spi_monitor_reset_channel_state(&g_spi_monitor_channels[channel]);
    }
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
    spi_monitor_packet_builder_t *builder,
    spi_monitor_channel_state_t *channel_state,
    spi_monitor_capture_t capture,
    uint8_t logical_channel,
    uint32_t timestamp_us
) {
    if (!builder->transaction_fragmented) {
        builder->transaction_sequence = channel_state->transactions_emitted + 1u;
    }

    builder->packet.header.version = TRACE_PACKET_VERSION;
    builder->packet.header.type = TRACE_TYPE_SPI;
    builder->packet.header.channel = logical_channel;
    builder->packet.header.flags = builder->pending_flags;
    builder->packet.header.payload_len = 0u;
    builder->packet.header.meta = (uint16_t)capture;
    builder->packet.header.sequence = builder->transaction_sequence;
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
    spi_monitor_packet_builder_t *builder,
    spi_monitor_channel_state_t *channel_state,
    uint8_t final_flags,
    bool end_of_transaction
) {
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
        builder->transaction_fragmented = true;
        spi_monitor_packet_builder_mark_next(builder, TRACE_FLAG_OVERFLOW);
        return false;
    }

    channel_state->packets_emitted += 1u;
    if (end_of_transaction) {
        channel_state->transactions_emitted += 1u;
    }
    builder->packet_open = false;
    builder->payload_offset = 0u;
    builder->transaction_fragmented = !end_of_transaction;
    if (end_of_transaction) {
        builder->transaction_sequence = 0u;
    }
    memset(&builder->packet, 0, sizeof(builder->packet));
    return true;
}

/** @brief Append one SPI data byte pair, emitting a fragment first if the next bytes would exceed @ref TRACE_PACKET_PAYLOAD_BYTES. */
static void spi_monitor_channel_append_byte_pair(
    spi_monitor_packet_builder_t *builder,
    spi_monitor_channel_state_t *channel_state,
    spi_monitor_capture_t capture,
    uint8_t logical_channel,
    uint32_t transaction_timestamp_us,
    uint8_t mosi_byte,
    uint8_t miso_byte
) {
    uint16_t bytes_needed = (capture == SPI_MONITOR_CAPTURE_MOSI_MISO) ? 2u : 1u;

    if (!builder->packet_open) {
        spi_monitor_packet_builder_begin(builder, channel_state, capture, logical_channel, transaction_timestamp_us);
    }

    if ((builder->payload_offset + bytes_needed) > TRACE_PACKET_PAYLOAD_BYTES) {
        (void)spi_monitor_packet_builder_flush(builder, channel_state, 0u, false);
        spi_monitor_packet_builder_begin(builder, channel_state, capture, logical_channel, transaction_timestamp_us);
    }

    builder->packet.payload[builder->payload_offset++] = mosi_byte;
    if (capture == SPI_MONITOR_CAPTURE_MOSI_MISO) {
        builder->packet.payload[builder->payload_offset++] = miso_byte;
    }
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

        (void)spi_monitor_packet_builder_flush(&bus_runtime->packet_builder, channel_state, closing_flags, true);
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
    bus_runtime->transaction_timestamp_us = timestamp_us;
    bus_runtime->last_activity_timestamp_us = timestamp_us;
}

/** @brief Extract one MOSI byte from a packed 16-bit raw sampler word. */
static uint8_t spi_monitor_extract_mosi_byte(uint32_t packed_samples) {
    return (uint8_t)(
        ((packed_samples >> 7u) & 0x80u) |
        ((packed_samples >> 6u) & 0x40u) |
        ((packed_samples >> 5u) & 0x20u) |
        ((packed_samples >> 4u) & 0x10u) |
        ((packed_samples >> 3u) & 0x08u) |
        ((packed_samples >> 2u) & 0x04u) |
        ((packed_samples >> 1u) & 0x02u) |
        (packed_samples & 0x01u)
    );
}

/** @brief Extract one MISO byte from a packed 16-bit raw sampler word. */
static uint8_t spi_monitor_extract_miso_byte(uint32_t packed_samples) {
    return (uint8_t)(
        ((packed_samples >> 8u) & 0x80u) |
        ((packed_samples >> 7u) & 0x40u) |
        ((packed_samples >> 6u) & 0x20u) |
        ((packed_samples >> 5u) & 0x10u) |
        ((packed_samples >> 4u) & 0x08u) |
        ((packed_samples >> 3u) & 0x04u) |
        ((packed_samples >> 2u) & 0x02u) |
        ((packed_samples >> 1u) & 0x01u)
    );
}

/** @brief Return the immutable sampler program template for one SPI mode. */
static const pio_program_t *spi_monitor_sampler_program_template(uint8_t spi_mode) {
    switch (spi_mode) {
        case 0u:
            return &spi_monitor_mode0_sampler_program;
        case 1u:
            return &spi_monitor_mode1_sampler_program;
        case 2u:
            return &spi_monitor_mode2_sampler_program;
        default:
            return &spi_monitor_mode3_sampler_program;
    }
}

/** @brief Rewrite wait instructions so one bus-local program copy watches the correct SCLK GPIO. */
static void spi_monitor_patch_sampler_waits(uint16_t *instructions, uint32_t clock_pin, uint8_t spi_mode) {
    switch (spi_mode) {
        case 0u:
            instructions[0] = pio_encode_wait_gpio(false, clock_pin);
            instructions[1] = pio_encode_wait_gpio(true, clock_pin);
            instructions[3] = pio_encode_wait_gpio(false, clock_pin);
            break;
        case 1u:
            instructions[0] = pio_encode_wait_gpio(false, clock_pin);
            instructions[1] = pio_encode_wait_gpio(true, clock_pin);
            instructions[2] = pio_encode_wait_gpio(false, clock_pin);
            break;
        case 2u:
            instructions[0] = pio_encode_wait_gpio(true, clock_pin);
            instructions[1] = pio_encode_wait_gpio(false, clock_pin);
            instructions[3] = pio_encode_wait_gpio(true, clock_pin);
            break;
        default:
            instructions[0] = pio_encode_wait_gpio(true, clock_pin);
            instructions[1] = pio_encode_wait_gpio(false, clock_pin);
            instructions[2] = pio_encode_wait_gpio(true, clock_pin);
            break;
    }
}

/** @brief Prepare one bus-local sampler program copy for the requested SPI mode. */
static bool spi_monitor_prepare_sampler_program(uint32_t sampler, uint8_t spi_mode) {
    const pio_program_t *template_program = spi_monitor_sampler_program_template(spi_mode);
    uint16_t *instructions = g_spi_monitor_program_instructions[sampler][spi_mode];
    uint8_t length;

    if ((template_program == NULL) || (template_program->instructions == NULL)) {
        return false;
    }

    length = template_program->length;
    if (length != SPI_MONITOR_PIO_PROGRAM_WORD_COUNT) {
        return false;
    }

    memset(instructions, 0, sizeof(g_spi_monitor_program_instructions[sampler][spi_mode]));
    memcpy(instructions, template_program->instructions, length * sizeof(uint16_t));
    spi_monitor_patch_sampler_waits(instructions, g_spi_monitor_bus_samplers[sampler].pin_base, spi_mode);

    g_spi_monitor_programs[sampler][spi_mode].instructions = instructions;
    g_spi_monitor_programs[sampler][spi_mode].length = length;
    g_spi_monitor_programs[sampler][spi_mode].origin = template_program->origin;
    return true;
}

/** @brief Decode one packed MOSI-only sample word for the active logical channel on a bus. */
static void spi_monitor_process_mosi_word(
    spi_monitor_bus_runtime_t *bus_runtime,
    spi_monitor_channel_state_t *channel_state,
    uint8_t logical_channel,
    uint32_t packed_samples
) {
    uint8_t mosi_byte = spi_monitor_extract_mosi_byte(packed_samples);

    spi_monitor_channel_append_byte_pair(
        &bus_runtime->packet_builder,
        channel_state,
        SPI_MONITOR_CAPTURE_MOSI,
        logical_channel,
        bus_runtime->transaction_timestamp_us,
        mosi_byte,
        0u
    );
}

/** @brief Decode one packed MOSI+MISO sample word for the active logical channel on a bus. */
static void spi_monitor_process_dual_word(
    spi_monitor_bus_runtime_t *bus_runtime,
    spi_monitor_channel_state_t *channel_state,
    uint8_t logical_channel,
    uint32_t packed_samples
) {
    uint8_t mosi_byte = spi_monitor_extract_mosi_byte(packed_samples);
    uint8_t miso_byte = spi_monitor_extract_miso_byte(packed_samples);

    spi_monitor_channel_append_byte_pair(
        &bus_runtime->packet_builder,
        channel_state,
        SPI_MONITOR_CAPTURE_MOSI_MISO,
        logical_channel,
        bus_runtime->transaction_timestamp_us,
        mosi_byte,
        miso_byte
    );
}

/** @brief Decode one raw SPI word buffer for the currently attributed logical channel on a bus. */
static void spi_monitor_decode_bus_words(
    spi_monitor_bus_runtime_t *bus_runtime,
    spi_monitor_channel_state_t *channel_state,
    spi_monitor_capture_t capture,
    uint8_t logical_channel,
    const uint32_t *raw_words,
    uint32_t raw_word_count
) {
    if (capture == SPI_MONITOR_CAPTURE_MOSI_MISO) {
        for (uint32_t word_index = 0u; word_index < raw_word_count; ++word_index) {
            spi_monitor_process_dual_word(bus_runtime, channel_state, logical_channel, raw_words[word_index]);
        }
        return;
    }

    for (uint32_t word_index = 0u; word_index < raw_word_count; ++word_index) {
        spi_monitor_process_mosi_word(bus_runtime, channel_state, logical_channel, raw_words[word_index]);
    }
}

/** @brief Decode one raw SPI sample buffer for one observed bus and attribute it to the active slot. */
void spi_monitor_internal_process_bus_words(
    uint32_t bus,
    uint8_t active_cs_mask,
    uint32_t timestamp_us,
    const uint32_t *raw_words,
    uint32_t raw_word_count
) {
    spi_monitor_bus_runtime_t *bus_runtime = &g_spi_monitor_bus_runtimes[bus];
    spi_monitor_capture_t capture = g_spi_monitor_buses[bus].capture;
    uint8_t active_slot;
    uint32_t channel;
    spi_monitor_channel_state_t *channel_state;
    uint8_t logical_channel;

    if ((raw_words == NULL) || (raw_word_count == 0u) || !g_spi_monitor_buses[bus].running) {
        return;
    }

    active_slot = spi_monitor_select_active_slot(bus, active_cs_mask);
    if (active_slot == SPI_MONITOR_MULTI_ACTIVE_SLOT) {
        spi_monitor_close_bus_transaction(bus, TRACE_FLAG_ERROR);
        return;
    }

    if (active_slot == SPI_MONITOR_NO_ACTIVE_SLOT) {
        if (!bus_runtime->transaction_open) {
            spi_monitor_close_bus_transaction(bus, 0u);
            return;
        }

        channel = spi_monitor_bus_slot_to_channel(bus, bus_runtime->active_slot);
        channel_state = &g_spi_monitor_channels[channel];
        logical_channel = g_spi_monitor_logical_channels[channel];

        spi_monitor_decode_bus_words(
            bus_runtime,
            channel_state,
            capture,
            logical_channel,
            raw_words,
            raw_word_count
        );

        bus_runtime->last_activity_timestamp_us = timestamp_us;
        return;
    }

    spi_monitor_open_bus_transaction(bus, active_slot, timestamp_us);
    channel = spi_monitor_bus_slot_to_channel(bus, bus_runtime->active_slot);
    channel_state = &g_spi_monitor_channels[channel];
    logical_channel = g_spi_monitor_logical_channels[channel];

    spi_monitor_decode_bus_words(
        bus_runtime,
        channel_state,
        capture,
        logical_channel,
        raw_words,
        raw_word_count
    );

    bus_runtime->last_activity_timestamp_us = timestamp_us;
}

/** @brief Decode one staged DMA half-buffer for one observed bus using the raw bus word buffer. */
static void spi_monitor_process_bus_half(
    uint32_t bus,
    uint32_t half_index,
    uint8_t active_cs_mask,
    uint32_t timestamp_us,
    uint32_t raw_word_count
) {
    spi_monitor_internal_process_bus_words(
        bus,
        active_cs_mask,
        timestamp_us,
        g_spi_monitor_bus_sampler_dma_buffers[bus][half_index],
        raw_word_count
    );
}

/** @brief Close timed-out SPI transactions whose chip-select did not explicitly end in time. */
void spi_monitor_internal_poll_bus_timeout(uint32_t bus, uint32_t now_us) {
    spi_monitor_bus_runtime_t *bus_runtime = &g_spi_monitor_bus_runtimes[bus];
    uint32_t timeout_us = g_spi_monitor_buses[bus].timeout_us;
    int32_t elapsed_us;

    if (!bus_runtime->transaction_open || (timeout_us == 0u)) {
        return;
    }

    elapsed_us = (int32_t)(now_us - bus_runtime->last_activity_timestamp_us);
    if (elapsed_us < 0) {
        return;
    }

    if ((uint32_t)elapsed_us >= timeout_us) {
        spi_monitor_close_bus_transaction(bus, 0u);
    }
}

/** @brief Clear volatile DMA-owned state for one observed SPI bus sampler. */
static void spi_monitor_reset_bus_sampler_state(spi_monitor_bus_sampler_state_t *sampler_state) {
    sampler_state->running = false;
    sampler_state->write_offset_words = 0u;
    sampler_state->last_transfer_count = 0u;
    sampler_state->overrun_count = 0u;
}

/** @brief Drop DMA progress accumulated while SPI trace output is disabled. */
static void spi_monitor_discard_bus_sampler_backlog(uint32_t sampler) {
    spi_monitor_bus_sampler_state_t *sampler_state = &g_spi_monitor_bus_samplers[sampler];
    uint32_t transfer_count;
    uint32_t words_written;

    if (!sampler_state->running) {
        return;
    }

    transfer_count = dma_channel_hw_addr((uint)sampler_state->dma_channel)->transfer_count;
    words_written = sampler_state->last_transfer_count - transfer_count;
    sampler_state->last_transfer_count = transfer_count;
    sampler_state->write_offset_words = (sampler_state->write_offset_words + (words_written % SPI_MONITOR_DMA_RING_WORDS)) % SPI_MONITOR_DMA_RING_WORDS;
}

/** @brief Return the sampler overrun count for one observed SPI bus. */
static uint32_t spi_monitor_bus_sampler_overrun_count(uint32_t bus) {
    const spi_monitor_bus_sampler_state_t *sampler_state = &g_spi_monitor_bus_samplers[bus];

    return sampler_state->running ? sampler_state->overrun_count : 0u;
}

void spi_monitor_internal_set_bus_sampler_overrun_count(uint32_t bus, uint32_t overrun_count) {
    g_spi_monitor_bus_samplers[bus].overrun_count = overrun_count;
}

bool spi_monitor_internal_test_stage_dma_progress(uint32_t bus, const uint32_t *raw_words, uint32_t raw_word_count) {
    spi_monitor_bus_sampler_state_t *sampler_state;

    if (!spi_monitor_internal_valid_bus(bus) || (raw_words == NULL) || (raw_word_count == 0u) || (raw_word_count > SPI_MONITOR_DMA_RING_WORDS)) {
        return false;
    }

    sampler_state = &g_spi_monitor_bus_samplers[bus];
    if (!sampler_state->running || (sampler_state->dma_channel < 0)) {
        return false;
    }

    memcpy(&g_spi_monitor_bus_sampler_dma_buffers[bus][0][0], raw_words, raw_word_count * sizeof(raw_words[0]));
    sampler_state->write_offset_words = 0u;
    sampler_state->last_transfer_count = UINT32_MAX;
    dma_channel_set_trans_count((uint)sampler_state->dma_channel, UINT32_MAX - raw_word_count, false);
    return true;
}

/** @brief Stop one observed SPI bus sampler and leave its DMA ring idle. */
static void spi_monitor_stop_bus_sampler(uint32_t sampler) {
    spi_monitor_bus_sampler_state_t *sampler_state = &g_spi_monitor_bus_samplers[sampler];

    if (sampler_state->dma_claimed) {
        dma_channel_abort((uint)sampler_state->dma_channel);
    }

    pio_sm_set_enabled(spi_monitor_pio, sampler_state->sm, false);
    pio_sm_clear_fifos(spi_monitor_pio, sampler_state->sm);
    spi_monitor_reset_bus_sampler_state(sampler_state);
    spi_monitor_set_poll_interest(sampler_state->bus, false);
}

/** @brief Start one observed SPI bus sampler with a continuous DMA ping-pong ring. */
static bool spi_monitor_start_bus_sampler(uint32_t sampler, uint8_t spi_mode) {
    spi_monitor_bus_sampler_state_t *sampler_state = &g_spi_monitor_bus_samplers[sampler];
    dma_channel_config dma_config;

    memset(g_spi_monitor_bus_sampler_dma_buffers[sampler], 0, sizeof(g_spi_monitor_bus_sampler_dma_buffers[sampler]));

    switch (spi_mode) {
        case 0u:
            spi_monitor_mode0_sampler_program_init(spi_monitor_pio, sampler_state->sm, g_spi_monitor_program_offsets[sampler][0], sampler_state->pin_base, 1.0f);
            break;
        case 1u:
            spi_monitor_mode1_sampler_program_init(spi_monitor_pio, sampler_state->sm, g_spi_monitor_program_offsets[sampler][1], sampler_state->pin_base, 1.0f);
            break;
        case 2u:
            spi_monitor_mode2_sampler_program_init(spi_monitor_pio, sampler_state->sm, g_spi_monitor_program_offsets[sampler][2], sampler_state->pin_base, 1.0f);
            break;
        default:
            spi_monitor_mode3_sampler_program_init(spi_monitor_pio, sampler_state->sm, g_spi_monitor_program_offsets[sampler][3], sampler_state->pin_base, 1.0f);
            break;
    }

    dma_config = dma_channel_get_default_config((uint)sampler_state->dma_channel);
    channel_config_set_transfer_data_size(&dma_config, DMA_SIZE_32);
    channel_config_set_read_increment(&dma_config, false);
    channel_config_set_write_increment(&dma_config, true);
    channel_config_set_dreq(&dma_config, pio_get_dreq(spi_monitor_pio, sampler_state->sm, false));
    channel_config_set_ring(&dma_config, true, SPI_MONITOR_DMA_RING_BITS);
    dma_channel_configure(
        (uint)sampler_state->dma_channel,
        &dma_config,
        &g_spi_monitor_bus_sampler_dma_buffers[sampler][0][0],
        &spi_monitor_pio->rxf[sampler_state->sm],
        UINT32_MAX,
        true
    );

    if (dma_channel_hw_addr((uint)sampler_state->dma_channel)->transfer_count != UINT32_MAX) {
        spi_monitor_reset_bus_sampler_state(sampler_state);
        return false;
    }

    sampler_state->running = true;
    sampler_state->write_offset_words = 0u;
    sampler_state->last_transfer_count = UINT32_MAX;
    sampler_state->overrun_count = 0u;
    spi_monitor_set_poll_interest(sampler_state->bus, true);
    return true;
}

/** @brief Start or stop the DMA-backed sampler that belongs to one observed SPI bus. */
static bool spi_monitor_apply_bus_capture(uint32_t bus, spi_monitor_capture_t capture, uint8_t spi_mode) {
    uint32_t sampler = bus;

    spi_monitor_stop_bus_sampler(sampler);
    if ((capture != SPI_MONITOR_CAPTURE_DISABLED) && !spi_monitor_start_bus_sampler(sampler, spi_mode)) {
        spi_monitor_stop_bus_sampler(sampler);
        return false;
    }

    return true;
}

/** @brief Stop one observed SPI bus and reset its runtime and control state. */
static void spi_monitor_stop_bus(uint32_t bus) {
    spi_monitor_close_bus_transaction(bus, 0u);
    spi_monitor_apply_bus_capture(bus, SPI_MONITOR_CAPTURE_DISABLED, 0u);
    spi_monitor_reset_bus_state(&g_spi_monitor_buses[bus]);
    spi_monitor_reset_bus_runtime(&g_spi_monitor_bus_runtimes[bus]);
}

/** @brief Consume one contiguous range of completed DMA words from one observed bus sampler. */
static void spi_monitor_consume_bus_sampler_words(uint32_t sampler, uint32_t word_offset, uint32_t word_count) {
    spi_monitor_bus_sampler_state_t *sampler_state = &g_spi_monitor_bus_samplers[sampler];
    const uint32_t *raw_words = &g_spi_monitor_bus_sampler_dma_buffers[sampler][0][0] + word_offset;

    if (word_count == 0u) {
        return;
    }

    /* ponytail: CS_N is sampled once per completed DMA half-buffer instead of per raw SPI bit.
     * That keeps the current clock-gated data-only sampler and one-sampler-per-bus DMA layout
     * intact, and it is acceptable for the current monitor model because a controller usually
     * leaves a clock gap when CS_N changes and starts the next transaction after that boundary.
     * If later hardware proves to switch CS_N mid-burst without a usable clock gap, upgrade the
     * raw sampler to carry CS bits in the DMA stream and decode those historical CS transitions
     * directly.
     */
    spi_monitor_internal_process_bus_words(
        sampler_state->bus,
        spi_monitor_sample_active_cs_mask(sampler_state->bus),
        spi_monitor_timestamp_us(),
        raw_words,
        word_count
    );
}

/** @brief Service DMA progress for one running observed bus sampler. */
static bool spi_monitor_poll_bus_sampler(uint32_t sampler) {
    spi_monitor_bus_sampler_state_t *sampler_state = &g_spi_monitor_bus_samplers[sampler];
    uint32_t transfer_count;
    uint32_t words_written;
    uint32_t offset;

    if (!sampler_state->running) {
        return false;
    }

    transfer_count = dma_channel_hw_addr((uint)sampler_state->dma_channel)->transfer_count;
    words_written = sampler_state->last_transfer_count - transfer_count;
    if (words_written == 0u) {
        return false;
    }

    sampler_state->last_transfer_count = transfer_count;
    if (words_written >= SPI_MONITOR_DMA_RING_WORDS) {
        sampler_state->overrun_count += (words_written / SPI_MONITOR_DMA_RING_WORDS);
        words_written %= SPI_MONITOR_DMA_RING_WORDS;
    }
    offset = sampler_state->write_offset_words;
    while (words_written != 0u) {
        uint32_t words_until_wrap = SPI_MONITOR_DMA_RING_WORDS - offset;
        uint32_t chunk_words = (words_written < words_until_wrap) ? words_written : words_until_wrap;

        spi_monitor_consume_bus_sampler_words(sampler, offset, chunk_words);
        offset = (offset + chunk_words) % SPI_MONITOR_DMA_RING_WORDS;
        words_written -= chunk_words;
    }

    sampler_state->write_offset_words = offset;
    return true;
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
    channel_state->packets_emitted = 0u;
    channel_state->transactions_emitted = 0u;
    channel_state->sink_overrun_count = 0u;
}

/** @brief Materialize one logical channel state into the public status snapshot. */
static void spi_monitor_fill_status(uint32_t channel, spi_monitor_channel_status_t *status_out) {
    const spi_monitor_channel_state_t *channel_state = &g_spi_monitor_channels[channel];
    uint32_t bus = spi_monitor_channel_to_bus(channel);
    bool running = spi_monitor_channel_running(channel);

    memset(status_out, 0, sizeof(*status_out));
    status_out->initialized = g_spi_monitor_initialized && !g_spi_monitor_init_failed;
    status_out->running = running;
    status_out->capture = running ? g_spi_monitor_buses[bus].capture : SPI_MONITOR_CAPTURE_DISABLED;
    status_out->spi_mode = running ? g_spi_monitor_buses[bus].spi_mode : 0u;
    status_out->timeout_us = running ? g_spi_monitor_buses[bus].timeout_us : 0u;
    status_out->packets_emitted = channel_state->packets_emitted;
    status_out->overrun_count = running ? channel_state->sink_overrun_count : 0u;
}

/** @brief Materialize one observed SPI bus state into the public bus status snapshot. */
static void spi_monitor_fill_bus_status(uint32_t bus, spi_monitor_bus_status_t *status_out) {
    const spi_monitor_bus_state_t *bus_state = &g_spi_monitor_buses[bus];
    uint32_t packets_emitted = 0u;
    uint32_t sink_overrun_count = 0u;
    uint32_t first_channel;
    uint32_t end_channel;

    spi_monitor_bus_channel_range(bus, &first_channel, &end_channel);
    for (uint32_t channel = first_channel; channel < end_channel; ++channel) {
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
    status_out->overrun_count = sink_overrun_count + spi_monitor_bus_sampler_overrun_count(bus);
}

/** @copydoc spi_monitor_init */
spi_monitor_rc_t spi_monitor_init(void) {
    uint32_t sampler;

    if (g_spi_monitor_initialized) {
        return SPI_MONITOR_RC_OK;
    }

    if (g_spi_monitor_init_failed) {
        return SPI_MONITOR_RC_FAILED;
    }

    memset(g_spi_monitor_channels, 0, sizeof(g_spi_monitor_channels));
    memset(g_spi_monitor_buses, 0, sizeof(g_spi_monitor_buses));
    memset(g_spi_monitor_bus_runtimes, 0, sizeof(g_spi_monitor_bus_runtimes));
    for (sampler = 0u; sampler < SPI_MONITOR_BUS_SAMPLER_COUNT; ++sampler) {
        spi_monitor_reset_bus_sampler_state(&g_spi_monitor_bus_samplers[sampler]);
        g_spi_monitor_bus_samplers[sampler].dma_channel = dma_claim_unused_channel(false);
        if (g_spi_monitor_bus_samplers[sampler].dma_channel < 0) {
            g_spi_monitor_init_failed = true;
            for (uint32_t claimed = 0u; claimed < sampler; ++claimed) {
                if (g_spi_monitor_bus_samplers[claimed].dma_claimed) {
                    dma_channel_unclaim((uint)g_spi_monitor_bus_samplers[claimed].dma_channel);
                    g_spi_monitor_bus_samplers[claimed].dma_claimed = false;
                    g_spi_monitor_bus_samplers[claimed].dma_channel = -1;
                }
            }
            return SPI_MONITOR_RC_FAILED;
        }
        g_spi_monitor_bus_samplers[sampler].dma_claimed = true;
    }
    for (sampler = 0u; sampler < SPI_MONITOR_BUS_SAMPLER_COUNT; ++sampler) {
        for (uint32_t spi_mode = 0u; spi_mode < SPI_MONITOR_PROGRAM_MODE_COUNT; ++spi_mode) {
            if (!spi_monitor_prepare_sampler_program(sampler, (uint8_t)spi_mode)) {
                g_spi_monitor_init_failed = true;
                for (uint32_t claimed = 0u; claimed < SPI_MONITOR_BUS_SAMPLER_COUNT; ++claimed) {
                    if (g_spi_monitor_bus_samplers[claimed].dma_claimed) {
                        dma_channel_unclaim((uint)g_spi_monitor_bus_samplers[claimed].dma_channel);
                        g_spi_monitor_bus_samplers[claimed].dma_claimed = false;
                        g_spi_monitor_bus_samplers[claimed].dma_channel = -1;
                    }
                }
                return SPI_MONITOR_RC_FAILED;
            }
            g_spi_monitor_program_offsets[sampler][spi_mode] = pio_add_program(spi_monitor_pio, &g_spi_monitor_programs[sampler][spi_mode]);
        }
    }
    g_spi_monitor_poll_bus_mask = 0u;
    g_spi_monitor_initialized = true;
    return SPI_MONITOR_RC_OK;
}

bool spi_monitor_needs_poll(void) {
    return g_spi_monitor_initialized && !g_spi_monitor_init_failed && (g_spi_monitor_poll_bus_mask != 0u);
}

/** @copydoc spi_monitor_poll */
void spi_monitor_poll(void) {
    bool stream_enabled = spi_monitor_internal_stream_enabled();

    if (!stream_enabled) {
        for (uint32_t bus = 0u; bus < SPI_MONITOR_BUS_COUNT; ++bus) {
            spi_monitor_internal_abort_bus_transaction(bus);
        }
        for (uint32_t sampler = 0u; sampler < SPI_MONITOR_BUS_SAMPLER_COUNT; ++sampler) {
            spi_monitor_discard_bus_sampler_backlog(sampler);
        }
        return;
    }

    for (uint32_t sampler = 0u; sampler < SPI_MONITOR_BUS_SAMPLER_COUNT; ++sampler) {
        (void)spi_monitor_poll_bus_sampler(sampler);
    }

    uint32_t now_us = spi_monitor_timestamp_us();
    for (uint32_t bus = 0u; bus < SPI_MONITOR_BUS_COUNT; ++bus) {
        spi_monitor_internal_poll_bus_timeout(bus, now_us);
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

    if (!spi_monitor_internal_valid_bus(bus) || (config == NULL)) {
        return SPI_MONITOR_RC_INVALID;
    }

    if (!spi_monitor_valid_capture(config->capture)) {
        return SPI_MONITOR_RC_INVALID;
    }

    if ((config->capture != SPI_MONITOR_CAPTURE_DISABLED) && !spi_monitor_internal_stream_enabled()) {
        return SPI_MONITOR_RC_DISABLED;
    }

    bus_state = &g_spi_monitor_buses[bus];
    spi_monitor_bus_channel_range(bus, &first_channel, &channel);
    if (config->capture == SPI_MONITOR_CAPTURE_DISABLED) {
        spi_monitor_stop_bus(bus);
        spi_monitor_reset_bus_channels(bus);
        return SPI_MONITOR_RC_OK;
    }

    if (!spi_monitor_valid_mode(config->spi_mode)) {
        return SPI_MONITOR_RC_INVALID;
    }

    if (!spi_monitor_valid_channel_select_mask(config->channel_select_mask)) {
        return SPI_MONITOR_RC_INVALID;
    }

    spi_monitor_stop_bus(bus);
    spi_monitor_reset_bus_channels(bus);
    timeout_us = (config->timeout_us != 0u) ? config->timeout_us : SPI_MONITOR_TIMEOUT_US_DEFAULT;
    if (!spi_monitor_apply_bus_capture(bus, config->capture, config->spi_mode)) {
        return SPI_MONITOR_RC_FAILED;
    }
    bus_state->running = true;
    bus_state->capture = config->capture;
    bus_state->spi_mode = config->spi_mode;
    bus_state->channel_select_mask = config->channel_select_mask;
    bus_state->timeout_us = timeout_us;
    spi_monitor_reset_bus_runtime(&g_spi_monitor_bus_runtimes[bus]);
    return SPI_MONITOR_RC_OK;
}

/** @copydoc spi_monitor_get_bus_status */
spi_monitor_rc_t spi_monitor_get_bus_status(uint32_t bus, spi_monitor_bus_status_t *status_out) {
    if (!spi_monitor_internal_valid_bus(bus) || (status_out == NULL)) {
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

