#ifndef SPI_MONITOR_PIO_H
#define SPI_MONITOR_PIO_H

#include "hardware/pio.h"

static const uint16_t g_spi_monitor_mode0_mosi_sampler_program_instructions[6] = {0u, 0u, 0u, 0u, 0u, 0u};
static const uint16_t g_spi_monitor_mode1_mosi_sampler_program_instructions[6] = {0u, 0u, 0u, 0u, 0u, 0u};
static const uint16_t g_spi_monitor_mode2_mosi_sampler_program_instructions[6] = {0u, 0u, 0u, 0u, 0u, 0u};
static const uint16_t g_spi_monitor_mode3_mosi_sampler_program_instructions[6] = {0u, 0u, 0u, 0u, 0u, 0u};
static const uint16_t g_spi_monitor_mode0_sampler_program_instructions[6] = {0u, 0u, 0u, 0u, 0u, 0u};
static const uint16_t g_spi_monitor_mode1_sampler_program_instructions[6] = {0u, 0u, 0u, 0u, 0u, 0u};
static const uint16_t g_spi_monitor_mode2_sampler_program_instructions[6] = {0u, 0u, 0u, 0u, 0u, 0u};
static const uint16_t g_spi_monitor_mode3_sampler_program_instructions[6] = {0u, 0u, 0u, 0u, 0u, 0u};

static const pio_program_t spi_monitor_mode0_mosi_sampler_program = {g_spi_monitor_mode0_mosi_sampler_program_instructions, 6u, -1};
static const pio_program_t spi_monitor_mode1_mosi_sampler_program = {g_spi_monitor_mode1_mosi_sampler_program_instructions, 6u, -1};
static const pio_program_t spi_monitor_mode2_mosi_sampler_program = {g_spi_monitor_mode2_mosi_sampler_program_instructions, 6u, -1};
static const pio_program_t spi_monitor_mode3_mosi_sampler_program = {g_spi_monitor_mode3_mosi_sampler_program_instructions, 6u, -1};
static const pio_program_t spi_monitor_mode0_sampler_program = {g_spi_monitor_mode0_sampler_program_instructions, 6u, -1};
static const pio_program_t spi_monitor_mode1_sampler_program = {g_spi_monitor_mode1_sampler_program_instructions, 6u, -1};
static const pio_program_t spi_monitor_mode2_sampler_program = {g_spi_monitor_mode2_sampler_program_instructions, 6u, -1};
static const pio_program_t spi_monitor_mode3_sampler_program = {g_spi_monitor_mode3_sampler_program_instructions, 6u, -1};

static inline void spi_monitor_mode0_mosi_sampler_program_init(PIO pio, uint sm, uint offset, uint data_pin_base, uint clock_pin, uint cs_gpio, float clkdiv) {
	(void)pio;
	(void)sm;
	(void)offset;
	(void)data_pin_base;
	(void)clock_pin;
	(void)cs_gpio;
	(void)clkdiv;
}

static inline void spi_monitor_mode1_mosi_sampler_program_init(PIO pio, uint sm, uint offset, uint data_pin_base, uint clock_pin, uint cs_gpio, float clkdiv) {
	(void)pio;
	(void)sm;
	(void)offset;
	(void)data_pin_base;
	(void)clock_pin;
	(void)cs_gpio;
	(void)clkdiv;
}

static inline void spi_monitor_mode2_mosi_sampler_program_init(PIO pio, uint sm, uint offset, uint data_pin_base, uint clock_pin, uint cs_gpio, float clkdiv) {
	(void)pio;
	(void)sm;
	(void)offset;
	(void)data_pin_base;
	(void)clock_pin;
	(void)cs_gpio;
	(void)clkdiv;
}

static inline void spi_monitor_mode3_mosi_sampler_program_init(PIO pio, uint sm, uint offset, uint data_pin_base, uint clock_pin, uint cs_gpio, float clkdiv) {
	(void)pio;
	(void)sm;
	(void)offset;
	(void)data_pin_base;
	(void)clock_pin;
	(void)cs_gpio;
	(void)clkdiv;
}

static inline void spi_monitor_mode0_sampler_program_init(PIO pio, uint sm, uint offset, uint data_pin_base, uint clock_pin, uint cs_gpio, float clkdiv) {
	(void)pio;
	(void)sm;
	(void)offset;
	(void)data_pin_base;
	(void)clock_pin;
	(void)cs_gpio;
	(void)clkdiv;
}

static inline void spi_monitor_mode1_sampler_program_init(PIO pio, uint sm, uint offset, uint data_pin_base, uint clock_pin, uint cs_gpio, float clkdiv) {
	(void)pio;
	(void)sm;
	(void)offset;
	(void)data_pin_base;
	(void)clock_pin;
	(void)cs_gpio;
	(void)clkdiv;
}

static inline void spi_monitor_mode2_sampler_program_init(PIO pio, uint sm, uint offset, uint data_pin_base, uint clock_pin, uint cs_gpio, float clkdiv) {
	(void)pio;
	(void)sm;
	(void)offset;
	(void)data_pin_base;
	(void)clock_pin;
	(void)cs_gpio;
	(void)clkdiv;
}

static inline void spi_monitor_mode3_sampler_program_init(PIO pio, uint sm, uint offset, uint data_pin_base, uint clock_pin, uint cs_gpio, float clkdiv) {
	(void)pio;
	(void)sm;
	(void)offset;
	(void)data_pin_base;
	(void)clock_pin;
	(void)cs_gpio;
	(void)clkdiv;
}

#endif