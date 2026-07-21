/**
 * @file spi_monitor.c
 * @brief SPI capture scaffold with DMA-backed per-channel sampling.
 */

#include "trace/capture/spi/spi_monitor.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/sync.h"
#include "pico/time.h"

#include "app_control.h"
#include "config/spi_monitor_config.h"
#include "trace/trace_ring.h"
#include "usb/usb_bulk.h"

#include "spi_monitor.pio.h"

#define SPI_MONITOR_PROGRAM_MODE_COUNT 4u
#define SPI_MONITOR_PIO_PROGRAM_WORD_COUNT 6u
#define SPI_MONITOR_DMA_HALF_COUNT 2u
#define SPI_MONITOR_DMA_RING_WORDS (SPI_MONITOR_DMA_BUFFER_WORDS * SPI_MONITOR_DMA_HALF_COUNT)
#define SPI_MONITOR_DMA_RING_BYTES (SPI_MONITOR_DMA_RING_WORDS * sizeof(uint32_t))
#define SPI_MONITOR_DMA_RING_BITS 8u
#define SPI_MONITOR_SAMPLES_PER_WORD 8u
#define SPI_MONITOR_RING_SATURATION_FREE_SLOTS 1u

typedef char spi_monitor_dma_ring_size_must_match[
    (SPI_MONITOR_DMA_RING_BYTES == (1u << SPI_MONITOR_DMA_RING_BITS)) ? 1 : -1
];
typedef char spi_monitor_pio_programs_must_fit[
    (SPI_MONITOR_CHANNEL_COUNT * SPI_MONITOR_PIO_PROGRAM_WORD_COUNT <= 32u) ? 1 : -1
];

#if defined(_MSC_VER)
#define SPI_MONITOR_DMA_ALIGN(bytes)
#else
#define SPI_MONITOR_DMA_ALIGN(bytes) __attribute__((aligned(bytes)))
#endif

/** @brief Packet-builder state for the active logical SPI transaction. */
typedef struct {
    bool packet_open; /**< Indicates whether a fixed trace packet fragment is currently open. */
    bool transaction_fragmented; /**< Indicates whether the current transaction already emitted an earlier fragment. */
    uint32_t transaction_sequence; /**< Sequence number assigned to the current logical SPI transaction. */
    uint8_t pending_flags; /**< Flags to apply to the next opened fragment. */
    uint16_t payload_offset; /**< Number of payload bytes already staged in the open fragment. */
    trace_packet_t packet; /**< Caller-owned fixed packet fragment under construction. */
} spi_monitor_packet_builder_t;

/** @brief Per-logical-channel accounting state retained across logical SPI transactions. */
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
    uint32_t ring_drop_count_base; /**< Baseline trace-ring drop count captured when the current session started. */
    uint32_t usb_host_backpressure_stall_count_base; /**< Baseline host-backpressure USB stall count captured when the current session started. */
    uint32_t usb_policy_deferral_count_base; /**< Baseline stream-policy deferral count captured when the current session started. */
    uint32_t peak_ring_depth_packets; /**< Peak queued trace-packet depth observed during the current session. */
    uint32_t timeout_close_count; /**< Number of channel transactions closed by the bus idle-timeout path during the current session. */
} spi_monitor_bus_state_t;

/** @brief Transaction decode state owned by one logical SPI channel. */
typedef struct {
    bool transaction_open; /**< Indicates whether one logical SPI transaction is currently open on this channel. */
    bool timeout_close_armed; /**< Indicates whether one prior idle timeout poll already observed this transaction stale without new DMA progress. */
    uint32_t transaction_timestamp_us; /**< Timestamp captured when the current logical transaction opened. */
    uint32_t last_activity_timestamp_us; /**< Timestamp of the most recently processed buffer attributed to this transaction. */
    spi_monitor_packet_builder_t packet_builder; /**< Fixed-packet builder that owns the active channel transaction fragment. */
} spi_monitor_channel_runtime_t;

/** @brief DMA-backed sampler state owned by one logical SPI channel. */
typedef struct {
    uint32_t bus; /**< Observed SPI bus index that owns this sampler. */
    PIO pio; /**< PIO block that owns this sampler state machine. */
    uint32_t clock_pin; /**< Observed bus SCLK GPIO used to clock this sampler. */
    uint32_t data_pin_base; /**< Contiguous sampled pin base containing `MOSI` and `MISO`. */
    uint32_t cs_gpio; /**< Observed active-low CS_N GPIO that gates this sampler. */
    uint sm; /**< PIO state machine dedicated to this raw bus sampler. */
    int dma_channel; /**< Claimed DMA channel for this raw channel sampler. */
    bool dma_claimed; /**< Indicates whether @ref dma_channel is reserved for this sampler. */
    bool running; /**< Indicates whether this raw channel sampler is currently active. */
    bool program_loaded; /**< Indicates whether this sampler currently has a PIO program installed. */
    uint32_t program_offset; /**< Program offset returned by @ref pio_add_program for the active mode. */
    uint32_t read_offset_words; /**< Oldest unread DMA word offset within the ping-pong ring. */
    uint32_t write_offset_words; /**< Most recently observed DMA write offset within the ping-pong ring. */
    uint32_t last_transfer_count; /**< Most recently observed DMA transfer count snapshot. */
    uint32_t overrun_count; /**< Number of completed half-buffers overwritten before service. */
    bool boundary_pending; /**< Indicates whether one exact CS boundary offset is waiting to split the current DMA span. */
    uint8_t boundary_offset; /**< Pending DMA write offset recorded at CS deassert when @ref boundary_pending is set. */
    uint16_t reserved0; /**< Reserved padding to keep the structure compact. */
} spi_monitor_channel_sampler_state_t;

/** @brief Logical channel identifiers exported on the shared trace packet header. */
static const uint8_t g_spi_monitor_logical_channels[SPI_MONITOR_CHANNEL_COUNT] = {
    SPI_MONITOR_CH0_LOGICAL_CHANNEL,
    SPI_MONITOR_CH1_LOGICAL_CHANNEL,
    SPI_MONITOR_CH2_LOGICAL_CHANNEL,
    SPI_MONITOR_CH3_LOGICAL_CHANNEL,
};
/** @brief Session state for every logical SPI channel. */
static spi_monitor_channel_state_t g_spi_monitor_channels[SPI_MONITOR_CHANNEL_COUNT];
/** @brief Shared runtime state for each observed SPI bus. */
static spi_monitor_bus_state_t g_spi_monitor_buses[SPI_MONITOR_BUS_COUNT];
/** @brief Decode state that tracks the current logical SPI transaction on each logical channel. */
static spi_monitor_channel_runtime_t g_spi_monitor_channel_runtimes[SPI_MONITOR_CHANNEL_COUNT];
/** @brief DMA-backed capture state for every logical SPI channel sampler. */
static spi_monitor_channel_sampler_state_t g_spi_monitor_channel_samplers[SPI_MONITOR_CHANNEL_COUNT] = {
    {.bus = 0u, .pio = pio0, .clock_pin = SPI_MONITOR_SPI0_SCLK_GPIO, .data_pin_base = SPI_MONITOR_SPI0_MOSI_GPIO, .cs_gpio = SPI_MONITOR_SPI0_CS0_GPIO, .sm = 0u, .dma_channel = -1},
    {.bus = 0u, .pio = pio0, .clock_pin = SPI_MONITOR_SPI0_SCLK_GPIO, .data_pin_base = SPI_MONITOR_SPI0_MOSI_GPIO, .cs_gpio = SPI_MONITOR_SPI0_CS1_GPIO, .sm = 1u, .dma_channel = -1},
    {.bus = 1u, .pio = pio0, .clock_pin = SPI_MONITOR_SPI1_SCLK_GPIO, .data_pin_base = SPI_MONITOR_SPI1_MOSI_GPIO, .cs_gpio = SPI_MONITOR_SPI1_CS0_GPIO, .sm = 2u, .dma_channel = -1},
    {.bus = 1u, .pio = pio0, .clock_pin = SPI_MONITOR_SPI1_SCLK_GPIO, .data_pin_base = SPI_MONITOR_SPI1_MOSI_GPIO, .cs_gpio = SPI_MONITOR_SPI1_CS1_GPIO, .sm = 3u, .dma_channel = -1},
};
/* ponytail: the test-only bus injector still fans one caller-provided raw-word buffer out to each
 * eligible active channel instead of simulating truly independent per-channel DMA retirement. That
 * keeps unit-test hooks small and is acceptable now because real capture ownership already moved to
 * one sampler plus DMA ring per logical stream. If bench behavior ever depends on skew between two
 * simultaneously active streams, extend the test hooks to inject per-channel DMA progress directly.
 */
/** @brief Ping-pong DMA storage for every logical SPI channel sampler. */
SPI_MONITOR_DMA_ALIGN(SPI_MONITOR_DMA_RING_BYTES)
static uint32_t g_spi_monitor_channel_sampler_dma_buffers[SPI_MONITOR_CHANNEL_COUNT][SPI_MONITOR_DMA_HALF_COUNT][SPI_MONITOR_DMA_BUFFER_WORDS];
/** @brief Mutable instruction storage for each channel-local active SPI sampler program copy. */
static uint16_t g_spi_monitor_program_instructions[SPI_MONITOR_CHANNEL_COUNT][SPI_MONITOR_PIO_PROGRAM_WORD_COUNT];
/** @brief Channel-local SPI sampler program copies patched to wait on the correct observed SCLK and CS_N GPIOs. */
static pio_program_t g_spi_monitor_programs[SPI_MONITOR_CHANNEL_COUNT];
/** @brief Indicates whether shared SPI monitor setup completed successfully. */
static bool g_spi_monitor_initialized;
/** @brief Sticky failure flag preventing repeated partial initialization attempts. */
static bool g_spi_monitor_init_failed;
/** @brief Bit mask of logical SPI samplers that are currently active. */
static uint8_t g_spi_monitor_active_sampler_mask;
/** @brief Bit mask of observed SPI buses that currently need producer-core polling. */
static uint8_t g_spi_monitor_poll_bus_mask;

/** @brief Lookup table that compacts alternating even-position sample bits from one byte into a nibble. */
static const uint8_t g_spi_monitor_compact_even_byte[256] = {
    0u, 0u, 1u, 1u, 0u, 0u, 1u, 1u,
    2u, 2u, 3u, 3u, 2u, 2u, 3u, 3u,
    0u, 0u, 1u, 1u, 0u, 0u, 1u, 1u,
    2u, 2u, 3u, 3u, 2u, 2u, 3u, 3u,
    4u, 4u, 5u, 5u, 4u, 4u, 5u, 5u,
    6u, 6u, 7u, 7u, 6u, 6u, 7u, 7u,
    4u, 4u, 5u, 5u, 4u, 4u, 5u, 5u,
    6u, 6u, 7u, 7u, 6u, 6u, 7u, 7u,
    0u, 0u, 1u, 1u, 0u, 0u, 1u, 1u,
    2u, 2u, 3u, 3u, 2u, 2u, 3u, 3u,
    0u, 0u, 1u, 1u, 0u, 0u, 1u, 1u,
    2u, 2u, 3u, 3u, 2u, 2u, 3u, 3u,
    4u, 4u, 5u, 5u, 4u, 4u, 5u, 5u,
    6u, 6u, 7u, 7u, 6u, 6u, 7u, 7u,
    4u, 4u, 5u, 5u, 4u, 4u, 5u, 5u,
    6u, 6u, 7u, 7u, 6u, 6u, 7u, 7u,
    8u, 8u, 9u, 9u, 8u, 8u, 9u, 9u,
    10u, 10u, 11u, 11u, 10u, 10u, 11u, 11u,
    8u, 8u, 9u, 9u, 8u, 8u, 9u, 9u,
    10u, 10u, 11u, 11u, 10u, 10u, 11u, 11u,
    12u, 12u, 13u, 13u, 12u, 12u, 13u, 13u,
    14u, 14u, 15u, 15u, 14u, 14u, 15u, 15u,
    12u, 12u, 13u, 13u, 12u, 12u, 13u, 13u,
    14u, 14u, 15u, 15u, 14u, 14u, 15u, 15u,
    8u, 8u, 9u, 9u, 8u, 8u, 9u, 9u,
    10u, 10u, 11u, 11u, 10u, 10u, 11u, 11u,
    8u, 8u, 9u, 9u, 8u, 8u, 9u, 9u,
    10u, 10u, 11u, 11u, 10u, 10u, 11u, 11u,
    12u, 12u, 13u, 13u, 12u, 12u, 13u, 13u,
    14u, 14u, 15u, 15u, 14u, 14u, 15u, 15u,
    12u, 12u, 13u, 13u, 12u, 12u, 13u, 13u,
    14u, 14u, 15u, 15u, 14u, 14u, 15u, 15u,
};
/** @brief Lookup table that compacts alternating odd-position sample bits from one byte into a nibble. */
static const uint8_t g_spi_monitor_compact_odd_byte[256] = {
    0u, 1u, 0u, 1u, 2u, 3u, 2u, 3u,
    0u, 1u, 0u, 1u, 2u, 3u, 2u, 3u,
    4u, 5u, 4u, 5u, 6u, 7u, 6u, 7u,
    4u, 5u, 4u, 5u, 6u, 7u, 6u, 7u,
    0u, 1u, 0u, 1u, 2u, 3u, 2u, 3u,
    0u, 1u, 0u, 1u, 2u, 3u, 2u, 3u,
    4u, 5u, 4u, 5u, 6u, 7u, 6u, 7u,
    4u, 5u, 4u, 5u, 6u, 7u, 6u, 7u,
    8u, 9u, 8u, 9u, 10u, 11u, 10u, 11u,
    8u, 9u, 8u, 9u, 10u, 11u, 10u, 11u,
    12u, 13u, 12u, 13u, 14u, 15u, 14u, 15u,
    12u, 13u, 12u, 13u, 14u, 15u, 14u, 15u,
    8u, 9u, 8u, 9u, 10u, 11u, 10u, 11u,
    8u, 9u, 8u, 9u, 10u, 11u, 10u, 11u,
    12u, 13u, 12u, 13u, 14u, 15u, 14u, 15u,
    12u, 13u, 12u, 13u, 14u, 15u, 14u, 15u,
    0u, 1u, 0u, 1u, 2u, 3u, 2u, 3u,
    0u, 1u, 0u, 1u, 2u, 3u, 2u, 3u,
    4u, 5u, 4u, 5u, 6u, 7u, 6u, 7u,
    4u, 5u, 4u, 5u, 6u, 7u, 6u, 7u,
    0u, 1u, 0u, 1u, 2u, 3u, 2u, 3u,
    0u, 1u, 0u, 1u, 2u, 3u, 2u, 3u,
    4u, 5u, 4u, 5u, 6u, 7u, 6u, 7u,
    4u, 5u, 4u, 5u, 6u, 7u, 6u, 7u,
    8u, 9u, 8u, 9u, 10u, 11u, 10u, 11u,
    8u, 9u, 8u, 9u, 10u, 11u, 10u, 11u,
    12u, 13u, 12u, 13u, 14u, 15u, 14u, 15u,
    12u, 13u, 12u, 13u, 14u, 15u, 14u, 15u,
    8u, 9u, 8u, 9u, 10u, 11u, 10u, 11u,
    8u, 9u, 8u, 9u, 10u, 11u, 10u, 11u,
    12u, 13u, 12u, 13u, 14u, 15u, 14u, 15u,
    12u, 13u, 12u, 13u, 14u, 15u, 14u, 15u,
};

static void spi_monitor_cs_irq_callback(uint gpio, uint32_t events);

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

/** @brief Return the bus-local chip-select slot for one logical SPI channel. */
static uint8_t spi_monitor_channel_to_slot(uint32_t channel) {
    return (uint8_t)(channel % SPI_MONITOR_CS_SLOTS_PER_BUS);
}

/** @brief Return the inclusive-exclusive logical channel range owned by one observed SPI bus. */
static void spi_monitor_bus_channel_range(uint32_t bus, uint32_t *first_channel, uint32_t *end_channel) {
    uint32_t first = spi_monitor_bus_first_channel(bus);

    *first_channel = first;
    *end_channel = first + SPI_MONITOR_CS_SLOTS_PER_BUS;
}

/** @brief Return the logical-sampler bit mask owned by one observed SPI bus. */
static uint8_t spi_monitor_bus_sampler_mask(uint32_t bus) {
    return (uint8_t)(SPI_MONITOR_CHANNEL_SELECT_ALL << spi_monitor_bus_first_channel(bus));
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

/** @brief Update the session peak for queued trace packets after one successful push. */
static void spi_monitor_note_bus_ring_depth(uint32_t bus);

/** @brief Clear one logical SPI channel back to the stopped session state. */
static void spi_monitor_reset_channel_state(spi_monitor_channel_state_t *channel_state);

/** @brief Extract one MOSI byte from a packed 16-bit raw sampler word. */
static uint8_t spi_monitor_extract_mosi_byte(uint32_t packed_samples);

/** @brief Extract one MISO byte from a packed 16-bit raw sampler word. */
static uint8_t spi_monitor_extract_miso_byte(uint32_t packed_samples);

/** @brief Extract one MOSI byte from a MOSI-only raw sampler word. */
static uint8_t spi_monitor_extract_mosi_only_byte(uint32_t raw_samples);

/** @brief Reset packet-builder state for one logical SPI channel. */
static void spi_monitor_packet_builder_reset(spi_monitor_packet_builder_t *builder) {
    builder->packet_open = false;
    builder->transaction_fragmented = false;
    builder->transaction_sequence = 0u;
    builder->pending_flags = 0u;
    builder->payload_offset = 0u;
    memset(&builder->packet.header, 0, sizeof(builder->packet.header));
}

/** @brief Drop only the currently open SPI fragment while preserving transaction-level continuation state. */
static void spi_monitor_packet_builder_drop_open_packet(spi_monitor_packet_builder_t *builder) {
    builder->packet_open = false;
    builder->payload_offset = 0u;
}

/** @brief Reset the active transaction state for one observed SPI bus. */
static void spi_monitor_reset_channel_runtime(spi_monitor_channel_runtime_t *channel_runtime) {
    memset(channel_runtime, 0, sizeof(*channel_runtime));
}

/** @brief Discard any open SPI packet builders owned by one logical channel. */
static void spi_monitor_discard_channel_packets(uint32_t channel) {
    spi_monitor_packet_builder_reset(&g_spi_monitor_channel_runtimes[channel].packet_builder);
}

/** @brief Abort one observed SPI bus transaction without flushing any partial fragment to the ring. */
void spi_monitor_internal_abort_bus_transaction(uint32_t bus) {
    uint32_t first_channel;
    uint32_t end_channel;

    spi_monitor_bus_channel_range(bus, &first_channel, &end_channel);
    for (uint32_t channel = first_channel; channel < end_channel; ++channel) {
        spi_monitor_discard_channel_packets(channel);
        spi_monitor_reset_channel_runtime(&g_spi_monitor_channel_runtimes[channel]);
    }
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
    return g_spi_monitor_channel_samplers[spi_monitor_bus_first_channel(bus) + slot].cs_gpio;
}

/** @brief Mark flags that should be applied to the next opened SPI packet fragment. */
static void spi_monitor_packet_builder_mark_next(spi_monitor_packet_builder_t *builder, uint8_t flags) {
    builder->pending_flags |= flags;
}

/** @brief Mark the current transaction as overflowed and count the transition only once until the next fragment opens. */
static void spi_monitor_packet_builder_note_overflow(
    spi_monitor_packet_builder_t *builder,
    spi_monitor_channel_state_t *channel_state
) {
    if ((builder->pending_flags & TRACE_FLAG_OVERFLOW) == 0u) {
        channel_state->sink_overrun_count += 1u;
    }

    spi_monitor_packet_builder_mark_next(builder, TRACE_FLAG_OVERFLOW);
}

/** @brief Return whether the shared trace ring is already saturated enough to avoid opening another continuation fragment. */
static bool spi_monitor_packet_builder_should_back_off(const spi_monitor_packet_builder_t *builder) {
    return builder->transaction_fragmented && (trace_ring_free() <= SPI_MONITOR_RING_SATURATION_FREE_SLOTS);
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
    uint32_t bus,
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
        spi_monitor_packet_builder_drop_open_packet(builder);
        builder->transaction_fragmented = true;
        spi_monitor_packet_builder_note_overflow(builder, channel_state);
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
    return true;
}

/** @brief Ensure the current SPI fragment is open and has room for the requested payload bytes. */
static bool spi_monitor_packet_builder_ensure_room(
    uint32_t bus,
    spi_monitor_packet_builder_t *builder,
    spi_monitor_channel_state_t *channel_state,
    spi_monitor_capture_t capture,
    uint8_t logical_channel,
    uint32_t transaction_timestamp_us,
    uint16_t bytes_needed
) {
    if (!builder->packet_open) {
        if (spi_monitor_packet_builder_should_back_off(builder)) {
            spi_monitor_packet_builder_note_overflow(builder, channel_state);
            return false;
        }
        spi_monitor_packet_builder_begin(builder, channel_state, capture, logical_channel, transaction_timestamp_us);
    }

    if ((builder->payload_offset + bytes_needed) > TRACE_PACKET_PAYLOAD_BYTES) {
        if (!spi_monitor_packet_builder_flush(bus, builder, channel_state, 0u, false)) {
            return false;
        }

        if (spi_monitor_packet_builder_should_back_off(builder)) {
            spi_monitor_packet_builder_note_overflow(builder, channel_state);
            return false;
        }

        spi_monitor_packet_builder_begin(builder, channel_state, capture, logical_channel, transaction_timestamp_us);
    }

    return true;
}

/** @brief Append one contiguous MOSI-only chunk without branching on capture mode in the inner loop. */
static void spi_monitor_append_mosi_words_chunk(
    spi_monitor_packet_builder_t *builder,
    const uint32_t *raw_words,
    uint32_t *word_index,
    uint32_t chunk_words
) {
    uint32_t chunk_end = *word_index + chunk_words;

    while (*word_index < chunk_end) {
        builder->packet.payload[builder->payload_offset++] = spi_monitor_extract_mosi_only_byte(raw_words[*word_index]);
        *word_index += 1u;
    }
}

/** @brief Append one contiguous MOSI+MISO chunk without branching on capture mode in the inner loop. */
static void spi_monitor_append_mosi_miso_words_chunk(
    spi_monitor_packet_builder_t *builder,
    const uint32_t *raw_words,
    uint32_t *word_index,
    uint32_t chunk_words
) {
    uint32_t chunk_end = *word_index + chunk_words;

    while (*word_index < chunk_end) {
        uint32_t packed_samples = raw_words[*word_index];

        builder->packet.payload[builder->payload_offset++] = spi_monitor_extract_mosi_byte(packed_samples);
        builder->packet.payload[builder->payload_offset++] = spi_monitor_extract_miso_byte(packed_samples);
        *word_index += 1u;
    }
}

/** @brief Append one contiguous span of SPI words in chunked fragments to reduce per-word packet bookkeeping. */
static void spi_monitor_channel_append_words(
    uint32_t bus,
    spi_monitor_packet_builder_t *builder,
    spi_monitor_channel_state_t *channel_state,
    uint8_t logical_channel,
    uint32_t transaction_timestamp_us,
    spi_monitor_capture_t capture,
    const uint32_t *raw_words,
    uint32_t raw_word_count
) {
    void (*append_chunk)(spi_monitor_packet_builder_t *, const uint32_t *, uint32_t *, uint32_t);
    uint32_t word_index = 0u;
    uint16_t bytes_per_word;

    if (capture == SPI_MONITOR_CAPTURE_MOSI_MISO) {
        bytes_per_word = 2u;
        append_chunk = spi_monitor_append_mosi_miso_words_chunk;
    } else {
        bytes_per_word = 1u;
        append_chunk = spi_monitor_append_mosi_words_chunk;
    }

    while (word_index < raw_word_count) {
        uint32_t chunk_words;
        uint32_t available_words;

        if (!spi_monitor_packet_builder_ensure_room(
            bus,
            builder,
            channel_state,
            capture,
            logical_channel,
            transaction_timestamp_us,
            bytes_per_word
        )) {
            break;
        }

        available_words = (TRACE_PACKET_PAYLOAD_BYTES - builder->payload_offset) / bytes_per_word;
        chunk_words = raw_word_count - word_index;
        if (chunk_words > available_words) {
            chunk_words = available_words;
        }

        append_chunk(builder, raw_words, &word_index, chunk_words);

        if ((builder->payload_offset == TRACE_PACKET_PAYLOAD_BYTES) && (word_index < raw_word_count)) {
            if (!spi_monitor_packet_builder_flush(bus, builder, channel_state, 0u, false)) {
                break;
            }
        }
    }
}

/** @brief Close the current logical transaction on one observed SPI bus. */
static void spi_monitor_close_channel_transaction(uint32_t channel, uint8_t closing_flags) {
    spi_monitor_channel_runtime_t *channel_runtime = &g_spi_monitor_channel_runtimes[channel];
    uint32_t bus = spi_monitor_channel_to_bus(channel);

    if (!channel_runtime->transaction_open) {
        spi_monitor_reset_channel_runtime(channel_runtime);
        return;
    }

    (void)spi_monitor_packet_builder_flush(
        bus,
        &channel_runtime->packet_builder,
        &g_spi_monitor_channels[channel],
        closing_flags,
        true
    );

    spi_monitor_reset_channel_runtime(channel_runtime);
}

/** @brief Close all open logical SPI transactions on one observed bus. */
static void spi_monitor_close_bus_transactions(uint32_t bus, uint8_t closing_flags) {
    uint32_t first_channel;
    uint32_t end_channel;

    spi_monitor_bus_channel_range(bus, &first_channel, &end_channel);
    for (uint32_t channel = first_channel; channel < end_channel; ++channel) {
        spi_monitor_close_channel_transaction(channel, closing_flags);
    }
}

/** @brief Ensure that one logical SPI channel has an open transaction. */
static void spi_monitor_open_channel_transaction(uint32_t channel, uint32_t timestamp_us) {
    spi_monitor_channel_runtime_t *channel_runtime = &g_spi_monitor_channel_runtimes[channel];

    if (channel_runtime->transaction_open) {
        channel_runtime->timeout_close_armed = false;
        channel_runtime->last_activity_timestamp_us = timestamp_us;
        return;
    }

    channel_runtime->transaction_open = true;
    channel_runtime->timeout_close_armed = false;
    channel_runtime->transaction_timestamp_us = timestamp_us;
    channel_runtime->last_activity_timestamp_us = timestamp_us;
}

/** @brief Extract one MOSI byte from a packed 16-bit raw sampler word. */
static uint8_t spi_monitor_extract_mosi_byte(uint32_t packed_samples) {
    uint8_t upper = (uint8_t)(packed_samples >> 8u);
    uint8_t lower = (uint8_t)packed_samples;

    return (uint8_t)(
        (g_spi_monitor_compact_odd_byte[upper] << 4u)
        | g_spi_monitor_compact_odd_byte[lower]
    );
}

/** @brief Extract one MISO byte from a packed 16-bit raw sampler word. */
static uint8_t spi_monitor_extract_miso_byte(uint32_t packed_samples) {
    uint8_t upper = (uint8_t)(packed_samples >> 8u);
    uint8_t lower = (uint8_t)packed_samples;

    return (uint8_t)(
        (g_spi_monitor_compact_even_byte[upper] << 4u)
        | g_spi_monitor_compact_even_byte[lower]
    );
}

/** @brief Extract one MOSI byte from a MOSI-only raw sampler word. */
static uint8_t spi_monitor_extract_mosi_only_byte(uint32_t raw_samples) {
    return (uint8_t)raw_samples;
}

/** @brief Return the immutable sampler program template for one SPI mode and capture direction. */
static const pio_program_t *spi_monitor_sampler_program_template(spi_monitor_capture_t capture, uint8_t spi_mode) {
    if (capture == SPI_MONITOR_CAPTURE_MOSI) {
        switch (spi_mode) {
            case 0u:
                return &spi_monitor_mode0_mosi_sampler_program;
            case 1u:
                return &spi_monitor_mode1_mosi_sampler_program;
            case 2u:
                return &spi_monitor_mode2_mosi_sampler_program;
            default:
                return &spi_monitor_mode3_mosi_sampler_program;
        }
    }

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

/** @brief Rewrite wait instructions so one channel-local program copy watches the correct SCLK and CS_N GPIOs. */
static void spi_monitor_patch_sampler_waits(uint16_t *instructions, uint32_t clock_pin, uint32_t cs_gpio, uint8_t spi_mode) {
    switch (spi_mode) {
        case 0u:
            instructions[0] = pio_encode_wait_gpio(false, cs_gpio);
            instructions[1] = pio_encode_wait_gpio(true, clock_pin);
            instructions[4] = pio_encode_wait_gpio(false, clock_pin);
            break;
        case 1u:
            instructions[0] = pio_encode_wait_gpio(false, cs_gpio);
            instructions[2] = pio_encode_wait_gpio(true, clock_pin);
            instructions[3] = pio_encode_wait_gpio(false, clock_pin);
            break;
        case 2u:
            instructions[0] = pio_encode_wait_gpio(false, cs_gpio);
            instructions[1] = pio_encode_wait_gpio(false, clock_pin);
            instructions[4] = pio_encode_wait_gpio(true, clock_pin);
            break;
        default:
            instructions[0] = pio_encode_wait_gpio(false, cs_gpio);
            instructions[2] = pio_encode_wait_gpio(false, clock_pin);
            instructions[3] = pio_encode_wait_gpio(true, clock_pin);
            break;
    }
}

/** @brief Return the current DMA write offset for one sampler from its transfer counter snapshot. */
static uint32_t spi_monitor_snapshot_channel_dma_write_offset(const spi_monitor_channel_sampler_state_t *sampler_state) {
    uint32_t transfer_count = dma_channel_hw_addr((uint)sampler_state->dma_channel)->transfer_count;
    uint32_t words_written = sampler_state->last_transfer_count - transfer_count;

    return (sampler_state->write_offset_words + (words_written % SPI_MONITOR_DMA_RING_WORDS)) % SPI_MONITOR_DMA_RING_WORDS;
}

/** @brief Clear the queued CS boundary offsets for one sampler. */
static void spi_monitor_reset_channel_boundary_queue(spi_monitor_channel_sampler_state_t *sampler_state) {
    sampler_state->boundary_pending = false;
    sampler_state->boundary_offset = 0u;
}

/** @brief Queue one exact CS boundary offset for a running sampler. */
static void spi_monitor_enqueue_channel_boundary(uint32_t sampler, uint32_t boundary_offset_words) {
    spi_monitor_channel_sampler_state_t *sampler_state = &g_spi_monitor_channel_samplers[sampler];
    uint8_t boundary_offset = (uint8_t)(boundary_offset_words % SPI_MONITOR_DMA_RING_WORDS);

    if (!sampler_state->running) {
        return;
    }

    if (sampler_state->boundary_pending) {
        uint32_t current_distance;
        uint32_t new_distance;

        if (sampler_state->boundary_offset == boundary_offset) {
            return;
        }

        current_distance = (sampler_state->boundary_offset + SPI_MONITOR_DMA_RING_WORDS - sampler_state->read_offset_words) % SPI_MONITOR_DMA_RING_WORDS;
        new_distance = (boundary_offset + SPI_MONITOR_DMA_RING_WORDS - sampler_state->read_offset_words) % SPI_MONITOR_DMA_RING_WORDS;
        if (new_distance < current_distance) {
            sampler_state->boundary_offset = boundary_offset;
        }
        return;
    }

    sampler_state->boundary_pending = true;
    sampler_state->boundary_offset = boundary_offset;
}

/** @brief Return the next queued CS boundary that falls within one contiguous DMA span. */
static bool spi_monitor_next_channel_boundary_in_span(
    const spi_monitor_channel_sampler_state_t *sampler_state,
    uint32_t span_start,
    uint32_t span_end,
    uint32_t *boundary_offset
) {
    if (!sampler_state->boundary_pending || (boundary_offset == NULL)) {
        return false;
    }

    if ((sampler_state->boundary_offset < span_start) || (sampler_state->boundary_offset > span_end)) {
        return false;
    }

    *boundary_offset = sampler_state->boundary_offset;
    return true;
}

/** @brief Consume the next queued CS boundary after its transaction close has been applied. */
static void spi_monitor_pop_channel_boundary(spi_monitor_channel_sampler_state_t *sampler_state) {
    if (!sampler_state->boundary_pending) {
        return;
    }

    sampler_state->boundary_pending = false;
}

/** @brief Latch the current DMA write position as a transaction boundary for one running sampler. */
static void spi_monitor_latch_channel_boundary(uint32_t sampler) {
    spi_monitor_channel_sampler_state_t *sampler_state = &g_spi_monitor_channel_samplers[sampler];
    uint32_t irq_state;
    uint32_t boundary_offset_words;

    if (!sampler_state->running || (sampler_state->dma_channel < 0)) {
        return;
    }

    irq_state = save_and_disable_interrupts();
    boundary_offset_words = spi_monitor_snapshot_channel_dma_write_offset(sampler_state);
    spi_monitor_enqueue_channel_boundary(sampler, boundary_offset_words);
    restore_interrupts(irq_state);
}

/** @brief Shared GPIO edge callback that latches CS deassert positions for running samplers. */
static void spi_monitor_cs_irq_callback(uint gpio, uint32_t events) {
    if ((events & GPIO_IRQ_EDGE_RISE) == 0u) {
        return;
    }

    for (uint32_t sampler = 0u; sampler < SPI_MONITOR_CHANNEL_COUNT; ++sampler) {
        if (g_spi_monitor_channel_samplers[sampler].cs_gpio == gpio) {
            spi_monitor_latch_channel_boundary(sampler);
            return;
        }
    }
}

/** @brief Decode one raw SPI word buffer for one logical channel whose sampler is already CS-gated. */
static void spi_monitor_internal_process_channel_words(
    uint32_t channel,
    uint32_t timestamp_us,
    const uint32_t *raw_words,
    uint32_t raw_word_count
) {
    spi_monitor_channel_runtime_t *channel_runtime = &g_spi_monitor_channel_runtimes[channel];
    spi_monitor_channel_state_t *channel_state = &g_spi_monitor_channels[channel];
    spi_monitor_packet_builder_t *builder = &channel_runtime->packet_builder;
    uint32_t bus = spi_monitor_channel_to_bus(channel);
    spi_monitor_capture_t capture = g_spi_monitor_buses[bus].capture;
    uint8_t logical_channel = g_spi_monitor_logical_channels[channel];
    uint32_t transaction_timestamp_us;

    if ((raw_words == NULL) || (raw_word_count == 0u) || !spi_monitor_channel_running(channel)) {
        return;
    }

    if (!channel_runtime->transaction_open) {
        spi_monitor_open_channel_transaction(channel, timestamp_us);
    }

    transaction_timestamp_us = channel_runtime->transaction_timestamp_us;
    channel_runtime->timeout_close_armed = false;

    spi_monitor_channel_append_words(
        bus,
        builder,
        channel_state,
        logical_channel,
        transaction_timestamp_us,
        capture,
        raw_words,
        raw_word_count
    );

    channel_runtime->last_activity_timestamp_us = timestamp_us;
}

/** @brief Close timed-out SPI transactions whose chip-select did not explicitly end in time. */
void spi_monitor_internal_poll_bus_timeout(uint32_t bus, uint32_t now_us) {
    uint32_t timeout_us = g_spi_monitor_buses[bus].timeout_us;
    uint32_t first_channel;
    uint32_t end_channel;

    if ((timeout_us == 0u) || !g_spi_monitor_buses[bus].running) {
        return;
    }

    spi_monitor_bus_channel_range(bus, &first_channel, &end_channel);
    for (uint32_t channel = first_channel; channel < end_channel; ++channel) {
        spi_monitor_channel_runtime_t *channel_runtime = &g_spi_monitor_channel_runtimes[channel];
        int32_t elapsed_us;

        if (!channel_runtime->transaction_open) {
            continue;
        }

        elapsed_us = (int32_t)(now_us - channel_runtime->last_activity_timestamp_us);
        if ((elapsed_us >= 0) && ((uint32_t)elapsed_us >= timeout_us)) {
            if (!channel_runtime->timeout_close_armed) {
                channel_runtime->timeout_close_armed = true;
                continue;
            }

            g_spi_monitor_buses[bus].timeout_close_count += 1u;
            spi_monitor_close_channel_transaction(channel, 0u);
        }
    }
}

/** @brief Clear volatile DMA-owned state for one logical SPI channel sampler. */
static void spi_monitor_reset_channel_sampler_state(spi_monitor_channel_sampler_state_t *sampler_state) {
    sampler_state->running = false;
    sampler_state->program_loaded = false;
    sampler_state->program_offset = 0u;
    sampler_state->read_offset_words = 0u;
    sampler_state->write_offset_words = 0u;
    sampler_state->last_transfer_count = 0u;
    sampler_state->overrun_count = 0u;
    spi_monitor_reset_channel_boundary_queue(sampler_state);
}

/** @brief Update active-sampler bookkeeping and derived bus poll-interest after one sampler changes state. */
static void spi_monitor_set_sampler_active(uint32_t sampler, bool active) {
    uint8_t sampler_mask = (uint8_t)(1u << sampler);
    uint32_t bus = g_spi_monitor_channel_samplers[sampler].bus;

    if (active) {
        g_spi_monitor_active_sampler_mask = (uint8_t)(g_spi_monitor_active_sampler_mask | sampler_mask);
    } else {
        g_spi_monitor_active_sampler_mask = (uint8_t)(g_spi_monitor_active_sampler_mask & (uint8_t)~sampler_mask);
    }

    spi_monitor_set_poll_interest(bus, (g_spi_monitor_active_sampler_mask & spi_monitor_bus_sampler_mask(bus)) != 0u);
}

/** @brief Drop DMA progress accumulated while SPI trace output is disabled. */
static void spi_monitor_discard_channel_sampler_backlog(uint32_t sampler) {
    spi_monitor_channel_sampler_state_t *sampler_state = &g_spi_monitor_channel_samplers[sampler];
    uint32_t transfer_count;
    uint32_t words_written;

    if (!sampler_state->running) {
        return;
    }

    transfer_count = dma_channel_hw_addr((uint)sampler_state->dma_channel)->transfer_count;
    words_written = sampler_state->last_transfer_count - transfer_count;
    sampler_state->last_transfer_count = transfer_count;
    sampler_state->read_offset_words = (sampler_state->read_offset_words + (words_written % SPI_MONITOR_DMA_RING_WORDS)) % SPI_MONITOR_DMA_RING_WORDS;
    sampler_state->write_offset_words = sampler_state->read_offset_words;
    spi_monitor_reset_channel_boundary_queue(sampler_state);
}

/** @brief Return the sampler overrun count for one observed SPI bus. */
static uint32_t spi_monitor_bus_sampler_overrun_count(uint32_t bus) {
    uint32_t first_channel;
    uint32_t end_channel;
    uint32_t overrun_count = 0u;

    spi_monitor_bus_channel_range(bus, &first_channel, &end_channel);
    for (uint32_t channel = first_channel; channel < end_channel; ++channel) {
        if (g_spi_monitor_channel_samplers[channel].running) {
            overrun_count += g_spi_monitor_channel_samplers[channel].overrun_count;
        }
    }

    return overrun_count;
}

void spi_monitor_internal_set_bus_sampler_overrun_count(uint32_t bus, uint32_t overrun_count) {
    uint32_t first_channel;
    uint32_t end_channel;
    uint32_t target_channel;

    spi_monitor_bus_channel_range(bus, &first_channel, &end_channel);
    for (uint32_t channel = first_channel; channel < end_channel; ++channel) {
        g_spi_monitor_channel_samplers[channel].overrun_count = 0u;
    }

    target_channel = first_channel;
    for (uint32_t channel = first_channel; channel < end_channel; ++channel) {
        if (g_spi_monitor_channel_samplers[channel].running) {
            target_channel = channel;
            break;
        }
    }

    g_spi_monitor_channel_samplers[target_channel].overrun_count = overrun_count;
}

bool spi_monitor_internal_test_stage_channel_dma_progress(uint32_t channel, const uint32_t *raw_words, uint32_t raw_word_count) {
    spi_monitor_channel_sampler_state_t *sampler_state;

    if ((channel >= SPI_MONITOR_CHANNEL_COUNT) || (raw_words == NULL) || (raw_word_count == 0u) || (raw_word_count > SPI_MONITOR_DMA_RING_WORDS)) {
        return false;
    }

    sampler_state = &g_spi_monitor_channel_samplers[channel];
    if (!sampler_state->running || (sampler_state->dma_channel < 0)) {
        return false;
    }

    memcpy(&g_spi_monitor_channel_sampler_dma_buffers[channel][0][0], raw_words, raw_word_count * sizeof(raw_words[0]));
    spi_monitor_reset_channel_boundary_queue(sampler_state);
    sampler_state->read_offset_words = 0u;
    sampler_state->write_offset_words = 0u;
    sampler_state->last_transfer_count = UINT32_MAX;
    dma_channel_set_trans_count((uint)sampler_state->dma_channel, UINT32_MAX - raw_word_count, false);
    return true;
}

bool spi_monitor_internal_test_stage_channel_dma_progress_with_boundary(
    uint32_t channel,
    const uint32_t *raw_words,
    uint32_t raw_word_count,
    uint32_t boundary_word_offset
) {
    if (!spi_monitor_internal_test_stage_channel_dma_progress(channel, raw_words, raw_word_count)) {
        return false;
    }

    if (boundary_word_offset > raw_word_count) {
        return false;
    }

    spi_monitor_enqueue_channel_boundary(channel, boundary_word_offset);
    return true;
}

/** @brief Stop one logical SPI channel sampler and leave its DMA ring idle. */
static void spi_monitor_stop_channel_sampler(uint32_t sampler) {
    spi_monitor_channel_sampler_state_t *sampler_state = &g_spi_monitor_channel_samplers[sampler];

    if (sampler_state->dma_claimed) {
        dma_channel_abort((uint)sampler_state->dma_channel);
    }

    pio_sm_set_enabled(sampler_state->pio, sampler_state->sm, false);
    pio_sm_clear_fifos(sampler_state->pio, sampler_state->sm);
    if (sampler_state->program_loaded) {
        pio_remove_program(sampler_state->pio, &g_spi_monitor_programs[sampler], sampler_state->program_offset);
    }
    spi_monitor_reset_channel_sampler_state(sampler_state);
    spi_monitor_set_sampler_active(sampler, false);
}

/** @brief Start one logical SPI channel sampler with a continuous DMA ping-pong ring. */
static bool spi_monitor_start_channel_sampler(uint32_t sampler, spi_monitor_capture_t capture, uint8_t spi_mode) {
    spi_monitor_channel_sampler_state_t *sampler_state = &g_spi_monitor_channel_samplers[sampler];
    dma_channel_config dma_config;
    uint16_t *instructions = g_spi_monitor_program_instructions[sampler];
    const pio_program_t *template_program = spi_monitor_sampler_program_template(capture, spi_mode);

    memset(g_spi_monitor_channel_sampler_dma_buffers[sampler], 0, sizeof(g_spi_monitor_channel_sampler_dma_buffers[sampler]));

    if ((template_program == NULL) || (template_program->instructions == NULL) || (template_program->length != SPI_MONITOR_PIO_PROGRAM_WORD_COUNT)) {
        return false;
    }

    memset(instructions, 0, sizeof(g_spi_monitor_program_instructions[sampler]));
    memcpy(instructions, template_program->instructions, template_program->length * sizeof(uint16_t));
    spi_monitor_patch_sampler_waits(instructions, sampler_state->clock_pin, sampler_state->cs_gpio, spi_mode);
    g_spi_monitor_programs[sampler].instructions = instructions;
    g_spi_monitor_programs[sampler].length = template_program->length;
    g_spi_monitor_programs[sampler].origin = template_program->origin;
    if (!pio_can_add_program(sampler_state->pio, &g_spi_monitor_programs[sampler])) {
        return false;
    }
    sampler_state->program_offset = pio_add_program(sampler_state->pio, &g_spi_monitor_programs[sampler]);
    sampler_state->program_loaded = true;

    if (capture == SPI_MONITOR_CAPTURE_MOSI) {
        switch (spi_mode) {
            case 0u:
                spi_monitor_mode0_mosi_sampler_program_init(sampler_state->pio, sampler_state->sm, sampler_state->program_offset, sampler_state->data_pin_base, sampler_state->clock_pin, sampler_state->cs_gpio, 1.0f);
                break;
            case 1u:
                spi_monitor_mode1_mosi_sampler_program_init(sampler_state->pio, sampler_state->sm, sampler_state->program_offset, sampler_state->data_pin_base, sampler_state->clock_pin, sampler_state->cs_gpio, 1.0f);
                break;
            case 2u:
                spi_monitor_mode2_mosi_sampler_program_init(sampler_state->pio, sampler_state->sm, sampler_state->program_offset, sampler_state->data_pin_base, sampler_state->clock_pin, sampler_state->cs_gpio, 1.0f);
                break;
            default:
                spi_monitor_mode3_mosi_sampler_program_init(sampler_state->pio, sampler_state->sm, sampler_state->program_offset, sampler_state->data_pin_base, sampler_state->clock_pin, sampler_state->cs_gpio, 1.0f);
                break;
        }
    } else {
        switch (spi_mode) {
            case 0u:
                spi_monitor_mode0_sampler_program_init(sampler_state->pio, sampler_state->sm, sampler_state->program_offset, sampler_state->data_pin_base, sampler_state->clock_pin, sampler_state->cs_gpio, 1.0f);
                break;
            case 1u:
                spi_monitor_mode1_sampler_program_init(sampler_state->pio, sampler_state->sm, sampler_state->program_offset, sampler_state->data_pin_base, sampler_state->clock_pin, sampler_state->cs_gpio, 1.0f);
                break;
            case 2u:
                spi_monitor_mode2_sampler_program_init(sampler_state->pio, sampler_state->sm, sampler_state->program_offset, sampler_state->data_pin_base, sampler_state->clock_pin, sampler_state->cs_gpio, 1.0f);
                break;
            default:
                spi_monitor_mode3_sampler_program_init(sampler_state->pio, sampler_state->sm, sampler_state->program_offset, sampler_state->data_pin_base, sampler_state->clock_pin, sampler_state->cs_gpio, 1.0f);
                break;
        }
    }

    dma_config = dma_channel_get_default_config((uint)sampler_state->dma_channel);
    channel_config_set_transfer_data_size(&dma_config, DMA_SIZE_32);
    channel_config_set_read_increment(&dma_config, false);
    channel_config_set_write_increment(&dma_config, true);
    channel_config_set_dreq(&dma_config, pio_get_dreq(sampler_state->pio, sampler_state->sm, false));
    channel_config_set_ring(&dma_config, true, SPI_MONITOR_DMA_RING_BITS);
    dma_channel_configure(
        (uint)sampler_state->dma_channel,
        &dma_config,
        &g_spi_monitor_channel_sampler_dma_buffers[sampler][0][0],
        &sampler_state->pio->rxf[sampler_state->sm],
        UINT32_MAX,
        true
    );

    if (dma_channel_hw_addr((uint)sampler_state->dma_channel)->transfer_count != UINT32_MAX) {
        pio_remove_program(sampler_state->pio, &g_spi_monitor_programs[sampler], sampler_state->program_offset);
        spi_monitor_reset_channel_sampler_state(sampler_state);
        return false;
    }

    sampler_state->running = true;
    sampler_state->read_offset_words = 0u;
    sampler_state->write_offset_words = 0u;
    sampler_state->last_transfer_count = UINT32_MAX;
    sampler_state->overrun_count = 0u;
    spi_monitor_reset_channel_boundary_queue(sampler_state);
    spi_monitor_set_sampler_active(sampler, true);
    return true;
}

/** @brief Start or stop the DMA-backed samplers that belong to one observed SPI bus. */
static bool spi_monitor_apply_bus_capture(uint32_t bus, spi_monitor_capture_t capture, uint8_t spi_mode, uint8_t channel_select_mask) {
    uint32_t first_channel;
    uint32_t end_channel;

    spi_monitor_bus_channel_range(bus, &first_channel, &end_channel);
    for (uint32_t channel = first_channel; channel < end_channel; ++channel) {
        spi_monitor_stop_channel_sampler(channel);
    }

    if (capture == SPI_MONITOR_CAPTURE_DISABLED) {
        return true;
    }

    for (uint32_t channel = first_channel; channel < end_channel; ++channel) {
        uint8_t slot_mask = (uint8_t)(1u << spi_monitor_channel_to_slot(channel));

        if ((channel_select_mask & slot_mask) == 0u) {
            continue;
        }

        if (!spi_monitor_start_channel_sampler(channel, capture, spi_mode)) {
            for (uint32_t rollback = first_channel; rollback <= channel; ++rollback) {
                spi_monitor_stop_channel_sampler(rollback);
            }
            return false;
        }
    }

    return true;
}

/** @brief Stop one observed SPI bus and reset its runtime and control state. */
static void spi_monitor_stop_bus(uint32_t bus) {
    uint32_t first_channel;
    uint32_t end_channel;

    spi_monitor_close_bus_transactions(bus, 0u);
    spi_monitor_apply_bus_capture(bus, SPI_MONITOR_CAPTURE_DISABLED, 0u, 0u);
    spi_monitor_reset_bus_state(&g_spi_monitor_buses[bus]);
    spi_monitor_bus_channel_range(bus, &first_channel, &end_channel);
    for (uint32_t channel = first_channel; channel < end_channel; ++channel) {
        spi_monitor_reset_channel_runtime(&g_spi_monitor_channel_runtimes[channel]);
    }
}

/** @brief Consume one contiguous range of completed DMA words from one logical SPI channel sampler. */
static void spi_monitor_consume_channel_sampler_words(uint32_t sampler, uint32_t word_offset, uint32_t word_count) {
    spi_monitor_channel_sampler_state_t *sampler_state = &g_spi_monitor_channel_samplers[sampler];
    const uint32_t *raw_words = &g_spi_monitor_channel_sampler_dma_buffers[sampler][0][0] + word_offset;
    uint32_t cursor = word_offset;
    uint32_t chunk_end = word_offset + word_count;

    if (word_count == 0u) {
        return;
    }

    while (cursor < chunk_end) {
        uint32_t boundary_offset;
        uint32_t span_end = chunk_end;

        if (spi_monitor_next_channel_boundary_in_span(sampler_state, cursor, chunk_end, &boundary_offset)) {
            span_end = boundary_offset;
        }

        if (span_end != cursor) {
            spi_monitor_internal_process_channel_words(
                sampler,
                spi_monitor_timestamp_us(),
                raw_words + (cursor - word_offset),
                span_end - cursor
            );
            cursor = span_end;
        }

        if (cursor == chunk_end) {
            break;
        }

        spi_monitor_close_channel_transaction(sampler, 0u);
        spi_monitor_pop_channel_boundary(sampler_state);
    }
}

/** @brief Close a transaction immediately when a latched CS boundary sits at the current DMA read position. */
static void spi_monitor_close_channel_sampler_at_idle_boundary(uint32_t sampler) {
    spi_monitor_channel_sampler_state_t *sampler_state = &g_spi_monitor_channel_samplers[sampler];

    if (!sampler_state->boundary_pending) {
        return;
    }

    if (sampler_state->boundary_offset != sampler_state->read_offset_words) {
        return;
    }

    spi_monitor_close_channel_transaction(sampler, 0u);
    spi_monitor_pop_channel_boundary(sampler_state);
}

/** @brief Service DMA progress for one running logical SPI channel sampler. */
static bool spi_monitor_poll_channel_sampler(uint32_t sampler) {
    spi_monitor_channel_sampler_state_t *sampler_state = &g_spi_monitor_channel_samplers[sampler];
    uint32_t irq_state;
    uint32_t transfer_count;
    uint32_t words_written;
    uint32_t offset;
    uint32_t snapshot_write_offset;

    if (!sampler_state->running) {
        return false;
    }

    irq_state = save_and_disable_interrupts();
    transfer_count = dma_channel_hw_addr((uint)sampler_state->dma_channel)->transfer_count;
    words_written = sampler_state->last_transfer_count - transfer_count;
    if (words_written == 0u) {
        restore_interrupts(irq_state);
        spi_monitor_close_channel_sampler_at_idle_boundary(sampler);
        return false;
    }

    sampler_state->last_transfer_count = transfer_count;
    if (words_written >= SPI_MONITOR_DMA_RING_WORDS) {
        sampler_state->overrun_count += (words_written / SPI_MONITOR_DMA_RING_WORDS);
        words_written %= SPI_MONITOR_DMA_RING_WORDS;
        spi_monitor_reset_channel_boundary_queue(sampler_state);
    }
    offset = sampler_state->read_offset_words;
    snapshot_write_offset = (sampler_state->write_offset_words + words_written) % SPI_MONITOR_DMA_RING_WORDS;
    sampler_state->write_offset_words = snapshot_write_offset;
    restore_interrupts(irq_state);

    while (words_written != 0u) {
        uint32_t words_until_wrap = SPI_MONITOR_DMA_RING_WORDS - offset;
        uint32_t chunk_words = (words_written < words_until_wrap) ? words_written : words_until_wrap;

        spi_monitor_consume_channel_sampler_words(sampler, offset, chunk_words);
        offset = (offset + chunk_words) % SPI_MONITOR_DMA_RING_WORDS;
        words_written -= chunk_words;
    }

    sampler_state->read_offset_words = offset;
    spi_monitor_close_channel_sampler_at_idle_boundary(sampler);
    return true;
}

/** @brief Clear the shared bus state back to the stopped session state. */
static void spi_monitor_reset_bus_state(spi_monitor_bus_state_t *bus_state) {
    bus_state->running = false;
    bus_state->capture = SPI_MONITOR_CAPTURE_DISABLED;
    bus_state->spi_mode = 0u;
    bus_state->channel_select_mask = 0u;
    bus_state->timeout_us = 0u;
    bus_state->ring_drop_count_base = 0u;
    bus_state->usb_host_backpressure_stall_count_base = 0u;
    bus_state->usb_policy_deferral_count_base = 0u;
    bus_state->peak_ring_depth_packets = 0u;
    bus_state->timeout_close_count = 0u;
}

/** @brief Update the session peak for queued trace packets from one producer-poll snapshot. */
static void spi_monitor_note_bus_ring_depth(uint32_t bus) {
    uint32_t ring_depth = trace_ring_available();

    if (ring_depth > g_spi_monitor_buses[bus].peak_ring_depth_packets) {
        g_spi_monitor_buses[bus].peak_ring_depth_packets = ring_depth;
    }
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
    uint32_t sampler_overrun_count = running ? g_spi_monitor_channel_samplers[channel].overrun_count : 0u;

    memset(status_out, 0, sizeof(*status_out));
    status_out->initialized = g_spi_monitor_initialized && !g_spi_monitor_init_failed;
    status_out->running = running;
    status_out->capture = running ? g_spi_monitor_buses[bus].capture : SPI_MONITOR_CAPTURE_DISABLED;
    status_out->spi_mode = running ? g_spi_monitor_buses[bus].spi_mode : 0u;
    status_out->timeout_us = running ? g_spi_monitor_buses[bus].timeout_us : 0u;
    status_out->packets_emitted = channel_state->packets_emitted;
    status_out->overrun_count = running ? (channel_state->sink_overrun_count + sampler_overrun_count) : 0u;
}

/** @brief Materialize one observed SPI bus state into the public bus status snapshot. */
static void spi_monitor_fill_bus_status(uint32_t bus, spi_monitor_bus_status_t *status_out) {
    const spi_monitor_bus_state_t *bus_state = &g_spi_monitor_buses[bus];
    uint32_t packets_emitted = 0u;
    uint32_t transactions_emitted = 0u;
    uint32_t sink_overrun_count = 0u;
    uint32_t sampler_overrun_count = spi_monitor_bus_sampler_overrun_count(bus);
    uint32_t first_channel;
    uint32_t end_channel;

    spi_monitor_bus_channel_range(bus, &first_channel, &end_channel);
    for (uint32_t channel = first_channel; channel < end_channel; ++channel) {
        packets_emitted += g_spi_monitor_channels[channel].packets_emitted;
        transactions_emitted += g_spi_monitor_channels[channel].transactions_emitted;
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
    status_out->transactions_emitted = transactions_emitted;
    status_out->overrun_count = sink_overrun_count + sampler_overrun_count;
    status_out->sink_overrun_count = sink_overrun_count;
    status_out->sampler_overrun_count = sampler_overrun_count;
    status_out->ring_drop_count = trace_ring_dropped_packets() - bus_state->ring_drop_count_base;
    status_out->usb_host_backpressure_stall_count = usb_bulk_host_backpressure_stall_count() - bus_state->usb_host_backpressure_stall_count_base;
    status_out->usb_policy_deferral_count = usb_bulk_policy_deferral_count() - bus_state->usb_policy_deferral_count_base;
    status_out->peak_ring_depth_packets = bus_state->peak_ring_depth_packets;
    status_out->timeout_close_count = bus_state->timeout_close_count;
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
    memset(g_spi_monitor_channel_runtimes, 0, sizeof(g_spi_monitor_channel_runtimes));
    gpio_set_irq_callback(spi_monitor_cs_irq_callback);
    for (sampler = 0u; sampler < SPI_MONITOR_CHANNEL_COUNT; ++sampler) {
        spi_monitor_reset_channel_sampler_state(&g_spi_monitor_channel_samplers[sampler]);
        g_spi_monitor_channel_samplers[sampler].dma_channel = dma_claim_unused_channel(false);
        if (g_spi_monitor_channel_samplers[sampler].dma_channel < 0) {
            g_spi_monitor_init_failed = true;
            for (uint32_t claimed = 0u; claimed < sampler; ++claimed) {
                if (g_spi_monitor_channel_samplers[claimed].dma_claimed) {
                    dma_channel_unclaim((uint)g_spi_monitor_channel_samplers[claimed].dma_channel);
                    g_spi_monitor_channel_samplers[claimed].dma_claimed = false;
                    g_spi_monitor_channel_samplers[claimed].dma_channel = -1;
                }
            }
            return SPI_MONITOR_RC_FAILED;
        }
        g_spi_monitor_channel_samplers[sampler].dma_claimed = true;
        gpio_set_irq_enabled(g_spi_monitor_channel_samplers[sampler].cs_gpio, GPIO_IRQ_EDGE_RISE, true);
    }
    g_spi_monitor_poll_bus_mask = 0u;
    g_spi_monitor_active_sampler_mask = 0u;
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
        uint8_t active_bus_mask = g_spi_monitor_poll_bus_mask;
        uint8_t active_sampler_mask = g_spi_monitor_active_sampler_mask;

        for (uint32_t bus = 0u; bus < SPI_MONITOR_BUS_COUNT; ++bus) {
            if ((active_bus_mask & (uint8_t)(1u << bus)) != 0u) {
                spi_monitor_internal_abort_bus_transaction(bus);
            }
        }
        for (uint32_t sampler = 0u; sampler < SPI_MONITOR_CHANNEL_COUNT; ++sampler) {
            if ((active_sampler_mask & (uint8_t)(1u << sampler)) != 0u) {
                spi_monitor_discard_channel_sampler_backlog(sampler);
            }
        }
        return;
    }

    for (uint32_t sampler = 0u; sampler < SPI_MONITOR_CHANNEL_COUNT; ++sampler) {
        if ((g_spi_monitor_active_sampler_mask & (uint8_t)(1u << sampler)) != 0u) {
            (void)spi_monitor_poll_channel_sampler(sampler);
        }
    }

    uint32_t now_us = spi_monitor_timestamp_us();
    for (uint32_t bus = 0u; bus < SPI_MONITOR_BUS_COUNT; ++bus) {
        if ((g_spi_monitor_poll_bus_mask & (uint8_t)(1u << bus)) == 0u) {
            continue;
        }

        spi_monitor_internal_poll_bus_timeout(bus, now_us);
        if (g_spi_monitor_buses[bus].running) {
            spi_monitor_note_bus_ring_depth(bus);
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
    if (!spi_monitor_apply_bus_capture(bus, config->capture, config->spi_mode, config->channel_select_mask)) {
        return SPI_MONITOR_RC_FAILED;
    }
    bus_state->running = true;
    bus_state->capture = config->capture;
    bus_state->spi_mode = config->spi_mode;
    bus_state->channel_select_mask = config->channel_select_mask;
    bus_state->timeout_us = timeout_us;
    bus_state->ring_drop_count_base = trace_ring_dropped_packets();
    bus_state->usb_host_backpressure_stall_count_base = usb_bulk_host_backpressure_stall_count();
    bus_state->usb_policy_deferral_count_base = usb_bulk_policy_deferral_count();
    bus_state->peak_ring_depth_packets = trace_ring_available();
    bus_state->timeout_close_count = 0u;
    for (channel = first_channel; channel < spi_monitor_bus_first_channel(bus) + SPI_MONITOR_CS_SLOTS_PER_BUS; ++channel) {
        spi_monitor_reset_channel_runtime(&g_spi_monitor_channel_runtimes[channel]);
    }
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

