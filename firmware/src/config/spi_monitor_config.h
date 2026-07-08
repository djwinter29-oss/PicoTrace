#ifndef PICO_SPI_MONITOR_CONFIG_H
#define PICO_SPI_MONITOR_CONFIG_H

#include <stdint.h>

/** @brief Number of observed SPI buses owned by the current board mapping. */
#define SPI_MONITOR_BUS_COUNT 2u
/** @brief Number of observed chip-select inputs per SPI bus. */
#define SPI_MONITOR_CS_SLOTS_PER_BUS 3u
/** @brief Total number of logical SPI channels exposed by the shared host-control model. */
#define SPI_MONITOR_CHANNEL_COUNT (SPI_MONITOR_BUS_COUNT * SPI_MONITOR_CS_SLOTS_PER_BUS)

/** @brief Default inter-byte timeout used when callers pass `0`. */
#define SPI_MONITOR_TIMEOUT_US_DEFAULT 1000u

/** @brief Observed `spi0` clock GPIO. */
#define SPI_MONITOR_SPI0_SCLK_GPIO 2u
/** @brief Observed `spi0` controller-to-peripheral data GPIO. */
#define SPI_MONITOR_SPI0_MOSI_GPIO 3u
/** @brief Observed `spi0` optional peripheral-to-controller data GPIO. */
#define SPI_MONITOR_SPI0_MISO_GPIO 4u
/** @brief Observed `spi0` chip-select slot `0` GPIO. */
#define SPI_MONITOR_SPI0_CS0_GPIO 5u
/** @brief Observed `spi0` chip-select slot `1` GPIO. */
#define SPI_MONITOR_SPI0_CS1_GPIO 6u
/** @brief Observed `spi0` chip-select slot `2` GPIO. */
#define SPI_MONITOR_SPI0_CS2_GPIO 7u

/** @brief Observed `spi1` chip-select slot `0` GPIO. */
#define SPI_MONITOR_SPI1_CS0_GPIO 8u
/** @brief Observed `spi1` chip-select slot `1` GPIO. */
#define SPI_MONITOR_SPI1_CS1_GPIO 9u
/** @brief Observed `spi1` chip-select slot `2` GPIO. */
#define SPI_MONITOR_SPI1_CS2_GPIO 10u
/** @brief Observed `spi1` clock GPIO. */
#define SPI_MONITOR_SPI1_SCLK_GPIO 11u
/** @brief Observed `spi1` controller-to-peripheral data GPIO. */
#define SPI_MONITOR_SPI1_MOSI_GPIO 12u
/** @brief Observed `spi1` optional peripheral-to-controller data GPIO. */
#define SPI_MONITOR_SPI1_MISO_GPIO 13u

/** @brief Logical SPI channel id for `spi0` chip-select slot `0`. */
#define SPI_MONITOR_CH0_LOGICAL_CHANNEL 0x00u
/** @brief Logical SPI channel id for `spi0` chip-select slot `1`. */
#define SPI_MONITOR_CH1_LOGICAL_CHANNEL 0x01u
/** @brief Logical SPI channel id for `spi0` chip-select slot `2`. */
#define SPI_MONITOR_CH2_LOGICAL_CHANNEL 0x02u
/** @brief Logical SPI channel id for `spi1` chip-select slot `0`. */
#define SPI_MONITOR_CH3_LOGICAL_CHANNEL 0x03u
/** @brief Logical SPI channel id for `spi1` chip-select slot `1`. */
#define SPI_MONITOR_CH4_LOGICAL_CHANNEL 0x04u
/** @brief Logical SPI channel id for `spi1` chip-select slot `2`. */
#define SPI_MONITOR_CH5_LOGICAL_CHANNEL 0x05u

#endif