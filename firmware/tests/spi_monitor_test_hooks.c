#include "spi_monitor_test_hooks.h"

#include <stddef.h>

#include "config/spi_monitor_config.h"

#define SPI_MONITOR_SAMPLES_PER_WORD 8u
#define SPI_MONITOR_BITS_PER_SAMPLE 2u

bool spi_monitor_internal_valid_bus(uint32_t bus);
bool spi_monitor_internal_stream_enabled(void);
void spi_monitor_internal_abort_bus_transaction(uint32_t bus);
void spi_monitor_internal_process_bus_words(
    uint32_t bus,
    uint8_t active_cs_mask,
    uint32_t timestamp_us,
    const uint32_t *raw_words,
    uint32_t raw_word_count
);
void spi_monitor_internal_poll_bus_timeout(uint32_t bus, uint32_t now_us);
void spi_monitor_internal_set_bus_sampler_overrun_count(uint32_t bus, uint32_t overrun_count);
bool spi_monitor_internal_test_stage_dma_progress(uint32_t bus, const uint32_t *raw_words, uint32_t raw_word_count);
bool spi_monitor_internal_test_stage_channel_dma_progress(uint32_t channel, const uint32_t *raw_words, uint32_t raw_word_count);
bool spi_monitor_internal_test_stage_channel_dma_progress_with_boundary(
    uint32_t channel,
    const uint32_t *raw_words,
    uint32_t raw_word_count,
    uint32_t boundary_word_offset
);

bool spi_monitor_test_feed_samples(
    uint32_t bus,
    uint8_t active_cs_mask,
    uint32_t timestamp_us,
    const uint32_t *raw_words,
    uint32_t raw_word_count
) {
    if (!spi_monitor_internal_valid_bus(bus) || (raw_words == NULL) || (raw_word_count == 0u)) {
        return false;
    }

    if (!spi_monitor_internal_stream_enabled()) {
        spi_monitor_internal_abort_bus_transaction(bus);
        return true;
    }

    spi_monitor_internal_process_bus_words(bus, active_cs_mask, timestamp_us, raw_words, raw_word_count);
    return true;
}

bool spi_monitor_test_feed_mosi_miso_samples(
    uint32_t bus,
    uint8_t active_cs_mask,
    uint32_t timestamp_us,
    const uint32_t *mosi_raw_words,
    const uint32_t *miso_raw_words,
    uint32_t raw_word_count
) {
    uint32_t merged_words[SPI_MONITOR_DMA_BUFFER_WORDS] = {0};

    if (!spi_monitor_internal_valid_bus(bus)
            || (mosi_raw_words == NULL)
            || (miso_raw_words == NULL)
            || (raw_word_count == 0u)
            || (raw_word_count > SPI_MONITOR_DMA_BUFFER_WORDS)) {
        return false;
    }

    if (!spi_monitor_internal_stream_enabled()) {
        spi_monitor_internal_abort_bus_transaction(bus);
        return true;
    }

    for (uint32_t word = 0u; word < raw_word_count; ++word) {
        uint32_t packed_mosi_samples = mosi_raw_words[word];
        uint32_t packed_miso_samples = miso_raw_words[word];

        for (uint32_t sample = 0u; sample < SPI_MONITOR_SAMPLES_PER_WORD; ++sample) {
            uint32_t sample_shift = 14u - (sample * SPI_MONITOR_BITS_PER_SAMPLE);
            uint8_t mosi_bit = (uint8_t)((packed_mosi_samples >> sample_shift) & 0x01u);
            uint8_t miso_bit = (uint8_t)((packed_miso_samples >> sample_shift) & 0x02u);

            merged_words[word] |= ((uint32_t)(mosi_bit | miso_bit)) << sample_shift;
        }
    }

    spi_monitor_internal_process_bus_words(bus, active_cs_mask, timestamp_us, merged_words, raw_word_count);
    return true;
}

void spi_monitor_test_poll_timeout(uint32_t bus, uint32_t timestamp_us) {
    if (!spi_monitor_internal_valid_bus(bus)) {
        return;
    }

    if (!spi_monitor_internal_stream_enabled()) {
        spi_monitor_internal_abort_bus_transaction(bus);
        return;
    }

    spi_monitor_internal_poll_bus_timeout(bus, timestamp_us);
}

void spi_monitor_test_set_bus_sampler_overrun_counts(uint32_t bus, uint32_t mosi_overruns, uint32_t miso_overruns) {
    if (!spi_monitor_internal_valid_bus(bus)) {
        return;
    }

    spi_monitor_internal_set_bus_sampler_overrun_count(bus, mosi_overruns + miso_overruns);
}

bool spi_monitor_test_stage_dma_progress(uint32_t bus, const uint32_t *raw_words, uint32_t raw_word_count) {
    return spi_monitor_internal_test_stage_dma_progress(bus, raw_words, raw_word_count);
}

bool spi_monitor_test_stage_channel_dma_progress(uint32_t channel, const uint32_t *raw_words, uint32_t raw_word_count) {
    return spi_monitor_internal_test_stage_channel_dma_progress(channel, raw_words, raw_word_count);
}

bool spi_monitor_test_stage_channel_dma_progress_with_boundary(
    uint32_t channel,
    const uint32_t *raw_words,
    uint32_t raw_word_count,
    uint32_t boundary_word_offset
) {
    return spi_monitor_internal_test_stage_channel_dma_progress_with_boundary(
        channel,
        raw_words,
        raw_word_count,
        boundary_word_offset
    );
}
