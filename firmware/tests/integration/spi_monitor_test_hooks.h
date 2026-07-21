#ifndef SPI_MONITOR_TEST_HOOKS_H
#define SPI_MONITOR_TEST_HOOKS_H

#include <stdbool.h>
#include <stdint.h>

bool spi_monitor_test_feed_samples(
    uint32_t bus,
    uint8_t active_cs_mask,
    uint32_t timestamp_us,
    const uint32_t *raw_words,
    uint32_t raw_word_count
);

bool spi_monitor_test_feed_mosi_miso_samples(
    uint32_t bus,
    uint8_t active_cs_mask,
    uint32_t timestamp_us,
    const uint32_t *mosi_raw_words,
    const uint32_t *miso_raw_words,
    uint32_t raw_word_count
);

void spi_monitor_test_poll_timeout(uint32_t bus, uint32_t timestamp_us);
void spi_monitor_test_set_bus_sampler_overrun_counts(uint32_t bus, uint32_t mosi_overruns, uint32_t miso_overruns);
bool spi_monitor_test_stage_channel_dma_progress(uint32_t channel, const uint32_t *raw_words, uint32_t raw_word_count);
bool spi_monitor_test_stage_channel_dma_progress_with_boundary(
    uint32_t channel,
    const uint32_t *raw_words,
    uint32_t raw_word_count,
    uint32_t boundary_word_offset
);

#endif
