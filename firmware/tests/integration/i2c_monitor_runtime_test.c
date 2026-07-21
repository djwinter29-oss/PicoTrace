#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "app_control.h"
#include "i2c_monitor_test_hooks.h"
#include "trace/capture/i2c/i2c_monitor.h"
#include "trace/decode/i2c/i2c_decoder.h"
#include "trace/trace_ring.h"

uint32_t stub_time_us32;
bool stub_dma_configure_fail_next;

void led_set(bool on) {
    (void)on;
}

void system_reboot(void) {
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

static void test_i2c_monitor_poll_drains_all_ready_buffers(void) {
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
    assert(status.completed_buffers == 4u);
    assert(status.overrun == false);
    assert(status.transition_pending == false);
}

int main(void) {
    test_i2c_monitor_poll_releases_completed_slot();
    test_i2c_monitor_backlog_overflow_latches_transition();
    test_i2c_monitor_poll_drains_all_ready_buffers();
    return 0;
}