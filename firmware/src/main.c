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
#include "trace/trace_ring.h"
#include "usb/usb_bulk.h"
#include "usb/usb_cdc.h"
#include "usb/usb_hid.h"

#define STREAM_SERVICE_PASSES 4u
#define STREAM_CONTROL_POLL_DIVIDER 8u

static uint32_t bridge_cli_read(void *context, uint8_t *data, uint32_t capacity) {
    (void)context;
    return usb_cdc_read(data, capacity);
}

static bool bridge_cli_write(void *context, const uint8_t *data, uint32_t length) {
    (void)context;
    return usb_cdc_write(data, length);
}

static const cli_shell_transport_t bridge_device_cli_transport = {
    .read = bridge_cli_read,
    .write = bridge_cli_write,
    .context = NULL,
};

static void trace_producer_core1_main(void) {
    bool init_ok = i2c_monitor_init();

    i2c_monitor_control_bind_executor(
        i2c_monitor_set_channel_sample_hz,
        i2c_monitor_get_channel_status
    );

    multicore_fifo_push_blocking(init_ok ? 1u : 0u);
    if (!init_ok) {
        while (true) {
            tight_loop_contents();
        }
    }

    while (true) {
        i2c_monitor_control_poll();
        tight_loop_contents();
    }
}

int main(void) {
    uint32_t control_poll_divider = 0u;

    system_init_clock();

    board_init();
    led_init();
    app_control_init();
    i2c_monitor_control_init();
    trace_ring_init();
    tusb_init();
    device_cli_init(&bridge_device_cli_transport);

    multicore_launch_core1(trace_producer_core1_main);
    if (multicore_fifo_pop_blocking() == 0u) {
        return 1;
    }

    while (true) {
        tud_task();
        if (tud_ready()) {
            for (uint32_t pass = 0u; pass < STREAM_SERVICE_PASSES; ++pass) {
                usb_bulk_poll_stream(app_control_stream_enabled());
                usb_bulk_flush();
            }
        }

        if (++control_poll_divider >= STREAM_CONTROL_POLL_DIVIDER) {
            control_poll_divider = 0u;
            device_cli_poll();
            usb_cdc_poll_tx();
            usb_hid_poll();
        }

        tight_loop_contents();
    }
}