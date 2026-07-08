#ifndef SPI_MONITOR_PIO_H
#define SPI_MONITOR_PIO_H

#include "hardware/pio.h"

static const pio_program_t spi_monitor_mode0_sampler_program = {0};
static const pio_program_t spi_monitor_mode1_sampler_program = {0};
static const pio_program_t spi_monitor_mode2_sampler_program = {0};
static const pio_program_t spi_monitor_mode3_sampler_program = {0};

static inline void spi_monitor_mode0_sampler_program_init(PIO pio, uint sm, uint offset, uint pin_base, float clkdiv) {
	(void)pio;
	(void)sm;
	(void)offset;
	(void)pin_base;
	(void)clkdiv;
}

static inline void spi_monitor_mode1_sampler_program_init(PIO pio, uint sm, uint offset, uint pin_base, float clkdiv) {
	(void)pio;
	(void)sm;
	(void)offset;
	(void)pin_base;
	(void)clkdiv;
}

static inline void spi_monitor_mode2_sampler_program_init(PIO pio, uint sm, uint offset, uint pin_base, float clkdiv) {
	(void)pio;
	(void)sm;
	(void)offset;
	(void)pin_base;
	(void)clkdiv;
}

static inline void spi_monitor_mode3_sampler_program_init(PIO pio, uint sm, uint offset, uint pin_base, float clkdiv) {
	(void)pio;
	(void)sm;
	(void)offset;
	(void)pin_base;
	(void)clkdiv;
}

#endif