/**
 * @file main.c
 * @brief Firmware entrypoint and top-level core ownership split for PicoTrace.
 */

#include "bsp/board.h"
#include "pico/multicore.h"
#include "tusb.h"

#include <stddef.h>

#include "app_control.h"
#include "cli/device_cli.h"
#include "driver/led.h"
#include "driver/system.h"
#include "trace/capture/i2c_monitor.h"
#include "trace/capture/i2c_monitor_control.h"
#include "trace/capture/spi_monitor.h"
#include "trace/capture/spi_monitor_control.h"
#include "trace/trace_ring.h"
#include "usb/usb_bulk.h"
#include "usb/usb_cdc.h"
#include "usb/usb_hid.h"

#define STREAM_SERVICE_PASSES 4u

/** @brief Bridge the CLI shell read callback onto the USB CDC transport. */
static uint32_t bridge_cli_read(void *context, uint8_t *data, uint32_t capacity) {
    (void)context;
    return usb_cdc_read(data, capacity);
}

/** @brief Bridge the CLI shell write callback onto the USB CDC transport. */
static bool bridge_cli_write(void *context, const uint8_t *data, uint32_t length) {
    (void)context;
    return usb_cdc_write(data, length);
}

/** @brief CLI transport adapter bound to the USB CDC command shell. */
static const cli_shell_transport_t bridge_device_cli_transport = {
    .read = bridge_cli_read,
    .write = bridge_cli_write,
    .context = NULL,
};

/**
 * @brief Core 1 entrypoint that owns trace production and monitor control execution.
 *
 * This core initializes the protocol monitor scaffolds, binds the producer-core control
 * executors, and then services both monitor control mailboxes plus producer-side polling forever.
 */
static void trace_producer_core1_main(void) {
    bool init_ok = (i2c_monitor_init() == I2C_MONITOR_RC_OK) && (spi_monitor_init() == SPI_MONITOR_RC_OK);

    i2c_monitor_control_bind_executor(
        i2c_monitor_set_channel_sample_hz,
        i2c_monitor_get_channel_status,
        i2c_monitor_get_all_status
    );
    spi_monitor_control_bind_executor(
        spi_monitor_set_bus_config,
        spi_monitor_get_bus_status,
        spi_monitor_get_all_status
    );

    multicore_fifo_push_blocking(init_ok ? 1u : 0u);
    if (!init_ok) {
        while (true) {
            tight_loop_contents();
        }
    }

    while (true) {
        i2c_monitor_control_poll();
        spi_monitor_control_poll();

        if (i2c_monitor_needs_poll()) {
            i2c_monitor_poll();
        }

        if (spi_monitor_needs_poll()) {
            spi_monitor_poll();
        }

        tight_loop_contents();
    }
}

/**
 * @brief Firmware entrypoint.
 * @return `0` is never returned during normal operation; `1` indicates producer-core init failure.
 */
int main(void) {
    system_init_clock();

    board_init();
    led_init();
    app_control_init();
    i2c_monitor_control_init();
    spi_monitor_control_init();
    trace_ring_init();
    tusb_init();
    device_cli_init(&bridge_device_cli_transport);

    multicore_reset_core1();
    multicore_launch_core1(trace_producer_core1_main);
    if (multicore_fifo_pop_blocking() == 0u) {
        return 1;
    }

    while (true) {
        tud_task();

        /* Service control paths before bulk streaming so CLI commands are responsive under load. */
        device_cli_poll();
        usb_cdc_poll_tx();
        usb_hid_poll();

        if (tud_ready()) {
            for (uint32_t pass = 0u; pass < STREAM_SERVICE_PASSES; ++pass) {
                usb_bulk_poll_stream(app_control_stream_enabled());
                usb_bulk_flush();

                /* Interleave control-path flushing with stream writes to avoid CDC starvation. */
                usb_cdc_poll_tx();
                usb_hid_poll();
            }
        }

        usb_cdc_poll_tx();

        tight_loop_contents();
    }
}