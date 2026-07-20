#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "hardware/clocks.h"
#include "app_control.h"
#include "i2c_monitor_test_hooks.h"
#include "trace/i2c/i2c_monitor.h"
#include "trace/i2c/i2c_decoder.h"
#include "trace/trace_ring.h"

uint32_t stub_time_us32;
bool stub_dma_configure_fail_next;

void led_set(bool on) {
    (void)on;
}

void system_reboot(void) {
}

static uint32_t test_expected_effective_sample_hz(uint32_t requested_sample_hz) {
    uint64_t scaled_sys_hz = ((uint64_t)clock_get_hz(clk_sys)) << 8u;
    uint32_t scaled_clkdiv = (uint32_t)((scaled_sys_hz + (requested_sample_hz / 2u)) / requested_sample_hz);

    if (scaled_clkdiv < 256u) {
        scaled_clkdiv = 256u;
    }

    return (uint32_t)((scaled_sys_hz + (scaled_clkdiv / 2u)) / scaled_clkdiv);
}

static void test_i2c_monitor_poll_releases_completed_slot(void) {
    static const uint32_t raw_words[1] = {0u};
    i2c_monitor_channel_status_t status;

    i2c_monitor_test_reset();
    trace_ring_init();
    app_control_init();

    assert(i2c_monitor_test_start_channel(0u, I2C_MONITOR_DEFAULT_SAMPLE_HZ) == true);
    assert(i2c_monitor_test_feed_completed_buffer(0u, raw_words, 1u) == true);
    assert(i2c_monitor_get_channel_status(0u, &status) == I2C_MONITOR_RC_OK);
    assert(status.completed_buffers == 1u);
    assert(status.overrun == false);
    assert(status.transition_pending == false);

    i2c_monitor_poll();

    assert(i2c_monitor_test_feed_completed_buffer(0u, raw_words, 1u) == true);
    assert(i2c_monitor_get_channel_status(0u, &status) == I2C_MONITOR_RC_OK);
    assert(status.completed_buffers == 2u);
    assert(status.overrun == false);
    assert(status.transition_pending == false);
}

static void test_i2c_monitor_backlog_overflow_latches_transition(void) {
    static const uint32_t raw_words[1] = {0u};
    i2c_monitor_channel_status_t status;

    i2c_monitor_test_reset();
    trace_ring_init();
    app_control_init();

    assert(i2c_monitor_test_start_channel(0u, I2C_MONITOR_DEFAULT_SAMPLE_HZ) == true);
    assert(i2c_monitor_test_feed_completed_buffer(0u, raw_words, 1u) == true);
    assert(i2c_monitor_test_feed_completed_buffer(0u, raw_words, 1u) == true);
    assert(i2c_monitor_test_feed_completed_buffer(0u, raw_words, 1u) == true);

    assert(i2c_monitor_get_channel_status(0u, &status) == I2C_MONITOR_RC_OK);
    assert(status.overrun == true);
    assert(status.running == false);
    assert(status.transition_pending == true);
    assert(status.transition_reason == I2C_DECODE_EVENT_OVERFLOW);
}

static void test_i2c_monitor_rejects_out_of_range_sample_hz(void) {
    i2c_monitor_channel_config_t config;

    i2c_monitor_test_reset();
    trace_ring_init();
    app_control_init();

    assert(i2c_monitor_init() == I2C_MONITOR_RC_OK);

    config.sample_hz = I2C_MONITOR_SAMPLE_HZ_SLOW - 1u;
    assert(i2c_monitor_set_channel_config(0u, &config) == I2C_MONITOR_RC_INVALID);

    config.sample_hz = 7700000u;
    assert(i2c_monitor_set_channel_config(0u, &config) == I2C_MONITOR_RC_INVALID);

    config.sample_hz = clock_get_hz(clk_sys) + 1u;
    assert(i2c_monitor_set_channel_config(0u, &config) == I2C_MONITOR_RC_INVALID);
}

static void test_i2c_monitor_status_reports_requested_and_effective_hz(void) {
    static const uint32_t requested_sample_hz = I2C_MONITOR_SAMPLE_HZ_FAST;
    i2c_monitor_channel_status_t status;

    i2c_monitor_test_reset();
    trace_ring_init();
    app_control_init();

    assert(i2c_monitor_test_start_channel(0u, requested_sample_hz) == true);
    assert(i2c_monitor_get_channel_status(0u, &status) == I2C_MONITOR_RC_OK);
    assert(status.requested_sample_hz == requested_sample_hz);
    assert(status.sample_hz == test_expected_effective_sample_hz(requested_sample_hz));
}

static void test_i2c_monitor_third_buffer_absorbs_one_extra_completion(void) {
    static const uint32_t raw_words[1] = {0u};
    i2c_monitor_channel_status_t status;

    i2c_monitor_test_reset();
    trace_ring_init();
    app_control_init();

    assert(i2c_monitor_test_start_channel(0u, I2C_MONITOR_DEFAULT_SAMPLE_HZ) == true);
    assert(i2c_monitor_test_feed_completed_buffer(0u, raw_words, 1u) == true);
    assert(i2c_monitor_test_feed_completed_buffer(0u, raw_words, 1u) == true);

    assert(i2c_monitor_get_channel_status(0u, &status) == I2C_MONITOR_RC_OK);
    assert(status.overrun == false);
    assert(status.running == true);
    assert(status.completed_buffers == 2u);
}

static void test_i2c_monitor_poll_drains_completed_backlog(void) {
    static const uint32_t raw_words[1] = {0u};
    i2c_monitor_channel_status_t status;

    i2c_monitor_test_reset();
    trace_ring_init();
    app_control_init();

    assert(i2c_monitor_test_start_channel(0u, I2C_MONITOR_DEFAULT_SAMPLE_HZ) == true);
    assert(i2c_monitor_test_feed_completed_buffer(0u, raw_words, 1u) == true);
    assert(i2c_monitor_test_feed_completed_buffer(0u, raw_words, 1u) == true);

    i2c_monitor_poll();

    assert(i2c_monitor_test_feed_completed_buffer(0u, raw_words, 1u) == true);
    assert(i2c_monitor_test_feed_completed_buffer(0u, raw_words, 1u) == true);
    assert(i2c_monitor_get_channel_status(0u, &status) == I2C_MONITOR_RC_OK);
    assert(status.overrun == false);
    assert(status.running == true);
    assert(status.completed_buffers == 4u);
}

int main(void) {
    test_i2c_monitor_poll_releases_completed_slot();
    test_i2c_monitor_backlog_overflow_latches_transition();
    test_i2c_monitor_rejects_out_of_range_sample_hz();
    test_i2c_monitor_status_reports_requested_and_effective_hz();
    test_i2c_monitor_third_buffer_absorbs_one_extra_completion();
    test_i2c_monitor_poll_drains_completed_backlog();
    return 0;
}