#include "trace/decode/i2c_decoder_test.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "trace/decode/i2c/i2c_decoder.h"

static void append_i2c_sample(uint8_t *samples, uint32_t *sample_count, bool sda, bool scl, uint32_t repeats) {
    uint8_t encoded = (uint8_t)((sda ? 0x01u : 0u) | (scl ? 0x02u : 0u));

    for (uint32_t index = 0u; index < repeats; ++index) {
        samples[*sample_count] = encoded;
        *sample_count += 1u;
    }
}

static void append_i2c_bit(uint8_t *samples, uint32_t *sample_count, uint8_t bit_value) {
    append_i2c_sample(samples, sample_count, bit_value != 0u, false, 2u);
    append_i2c_sample(samples, sample_count, bit_value != 0u, true, 2u);
    append_i2c_sample(samples, sample_count, bit_value != 0u, false, 2u);
}

static uint32_t pack_i2c_samples(uint32_t *raw_words, uint32_t raw_capacity, const uint8_t *samples, uint32_t sample_count) {
    uint32_t word_count = (sample_count + 15u) / 16u;

    assert(word_count <= raw_capacity);
    memset(raw_words, 0, raw_capacity * sizeof(raw_words[0]));

    for (uint32_t sample_index = 0u; sample_index < sample_count; ++sample_index) {
        uint32_t word_index = sample_index / 16u;
        uint32_t shift = 30u - ((sample_index & 0x0Fu) * 2u);
        raw_words[word_index] |= ((uint32_t)(samples[sample_index] & 0x03u) << shift);
    }

    return word_count;
}

typedef struct {
    i2c_decode_event_t events[32];
    uint32_t count;
} test_i2c_event_capture_t;

typedef struct {
    test_i2c_event_capture_t capture;
    uint32_t reject_after;
} test_i2c_event_reject_capture_t;

static bool capture_i2c_event(void *context, uint8_t event_type, uint8_t event_value) {
    test_i2c_event_capture_t *capture = (test_i2c_event_capture_t *)context;

    assert(capture->count < 32u);
    capture->events[capture->count].type = event_type;
    capture->events[capture->count].value = event_value;
    capture->count += 1u;
    return true;
}

static bool capture_i2c_event_with_reject_after(void *context, uint8_t event_type, uint8_t event_value) {
    test_i2c_event_reject_capture_t *capture = (test_i2c_event_reject_capture_t *)context;

    if (capture->capture.count >= capture->reject_after) {
        return false;
    }

    return capture_i2c_event(&capture->capture, event_type, event_value);
}

static void test_i2c_decoder_emits_events_from_oversampled_buffer(void) {
    i2c_decoder_state_t decoder_state;
    uint8_t samples[256];
    uint32_t raw_words[16];
    uint32_t sample_count = 0u;
    uint32_t raw_word_count;
    test_i2c_event_capture_t capture = {0};

    i2c_decoder_init(&decoder_state);

    append_i2c_sample(samples, &sample_count, true, true, 3u);
    append_i2c_sample(samples, &sample_count, false, true, 3u);

    append_i2c_bit(samples, &sample_count, 1u);
    append_i2c_bit(samples, &sample_count, 0u);
    append_i2c_bit(samples, &sample_count, 1u);
    append_i2c_bit(samples, &sample_count, 0u);
    append_i2c_bit(samples, &sample_count, 0u);
    append_i2c_bit(samples, &sample_count, 0u);
    append_i2c_bit(samples, &sample_count, 0u);
    append_i2c_bit(samples, &sample_count, 0u);
    append_i2c_bit(samples, &sample_count, 0u);

    append_i2c_bit(samples, &sample_count, 0u);
    append_i2c_bit(samples, &sample_count, 1u);
    append_i2c_bit(samples, &sample_count, 0u);
    append_i2c_bit(samples, &sample_count, 1u);
    append_i2c_bit(samples, &sample_count, 1u);
    append_i2c_bit(samples, &sample_count, 0u);
    append_i2c_bit(samples, &sample_count, 1u);
    append_i2c_bit(samples, &sample_count, 0u);
    append_i2c_bit(samples, &sample_count, 0u);

    append_i2c_sample(samples, &sample_count, false, false, 2u);
    append_i2c_sample(samples, &sample_count, false, true, 2u);
    append_i2c_sample(samples, &sample_count, true, true, 3u);

    raw_word_count = pack_i2c_samples(raw_words, 16u, samples, sample_count);

    assert(i2c_decoder_process_buffer(&decoder_state, raw_words, raw_word_count, capture_i2c_event, &capture) == I2C_DECODER_RESULT_OK);
    assert(capture.count == 6u);
    assert(capture.events[0].type == I2C_DECODE_EVENT_START);
    assert(capture.events[1].type == I2C_DECODE_EVENT_DATA);
    assert(capture.events[1].value == 0xA0u);
    assert(capture.events[2].type == I2C_DECODE_EVENT_ACK);
    assert(capture.events[2].value == 0u);
    assert(capture.events[3].type == I2C_DECODE_EVENT_DATA);
    assert(capture.events[3].value == 0x5Au);
    assert(capture.events[4].type == I2C_DECODE_EVENT_ACK);
    assert(capture.events[4].value == 0u);
    assert(capture.events[5].type == I2C_DECODE_EVENT_STOP);
}

static void test_i2c_decoder_carries_transaction_across_buffers(void) {
    i2c_decoder_state_t decoder_state;
    uint8_t first_samples[64];
    uint8_t second_samples[192];
    uint32_t first_sample_count = 0u;
    uint32_t second_sample_count = 0u;
    uint32_t first_raw_words[8];
    uint32_t second_raw_words[16];
    uint32_t first_raw_word_count;
    uint32_t second_raw_word_count;
    test_i2c_event_capture_t capture = {0};

    i2c_decoder_init(&decoder_state);

    append_i2c_sample(first_samples, &first_sample_count, true, true, 3u);
    append_i2c_sample(first_samples, &first_sample_count, false, true, 3u);
    append_i2c_bit(first_samples, &first_sample_count, 1u);
    append_i2c_bit(first_samples, &first_sample_count, 0u);
    append_i2c_bit(first_samples, &first_sample_count, 1u);
    append_i2c_bit(first_samples, &first_sample_count, 0u);

    append_i2c_bit(second_samples, &second_sample_count, 0u);
    append_i2c_bit(second_samples, &second_sample_count, 0u);
    append_i2c_bit(second_samples, &second_sample_count, 0u);
    append_i2c_bit(second_samples, &second_sample_count, 0u);
    append_i2c_bit(second_samples, &second_sample_count, 0u);

    append_i2c_bit(second_samples, &second_sample_count, 0u);
    append_i2c_bit(second_samples, &second_sample_count, 1u);
    append_i2c_bit(second_samples, &second_sample_count, 0u);
    append_i2c_bit(second_samples, &second_sample_count, 1u);
    append_i2c_bit(second_samples, &second_sample_count, 1u);
    append_i2c_bit(second_samples, &second_sample_count, 0u);
    append_i2c_bit(second_samples, &second_sample_count, 1u);
    append_i2c_bit(second_samples, &second_sample_count, 0u);
    append_i2c_bit(second_samples, &second_sample_count, 0u);

    append_i2c_sample(second_samples, &second_sample_count, false, false, 2u);
    append_i2c_sample(second_samples, &second_sample_count, false, true, 2u);
    append_i2c_sample(second_samples, &second_sample_count, true, true, 3u);

    first_raw_word_count = pack_i2c_samples(first_raw_words, 8u, first_samples, first_sample_count);
    second_raw_word_count = pack_i2c_samples(second_raw_words, 16u, second_samples, second_sample_count);

    assert(i2c_decoder_process_buffer(&decoder_state, first_raw_words, first_raw_word_count, capture_i2c_event, &capture) == I2C_DECODER_RESULT_OK);
    assert(capture.count == 1u);
    assert(capture.events[0].type == I2C_DECODE_EVENT_START);

    assert(i2c_decoder_process_buffer(&decoder_state, second_raw_words, second_raw_word_count, capture_i2c_event, &capture) == I2C_DECODER_RESULT_OK);
    assert(capture.count == 6u);
    assert(capture.events[1].type == I2C_DECODE_EVENT_DATA);
    assert(capture.events[1].value == 0xA0u);
    assert(capture.events[2].type == I2C_DECODE_EVENT_ACK);
    assert(capture.events[2].value == 0u);
    assert(capture.events[3].type == I2C_DECODE_EVENT_DATA);
    assert(capture.events[3].value == 0x5Au);
    assert(capture.events[4].type == I2C_DECODE_EVENT_ACK);
    assert(capture.events[4].value == 0u);
    assert(capture.events[5].type == I2C_DECODE_EVENT_STOP);
}

static void test_i2c_decoder_preserves_progress_after_sink_rejection(void) {
    i2c_decoder_state_t decoder_state;
    uint8_t first_samples[128];
    uint8_t second_samples[128];
    uint32_t first_sample_count = 0u;
    uint32_t second_sample_count = 0u;
    uint32_t first_raw_words[8];
    uint32_t second_raw_words[8];
    uint32_t first_raw_word_count;
    uint32_t second_raw_word_count;
    test_i2c_event_reject_capture_t first_capture = {0};
    test_i2c_event_capture_t second_capture = {0};

    i2c_decoder_init(&decoder_state);
    first_capture.reject_after = 1u;

    append_i2c_sample(first_samples, &first_sample_count, true, true, 3u);
    append_i2c_sample(first_samples, &first_sample_count, false, true, 3u);
    append_i2c_bit(first_samples, &first_sample_count, 1u);
    append_i2c_bit(first_samples, &first_sample_count, 0u);
    append_i2c_bit(first_samples, &first_sample_count, 1u);
    append_i2c_bit(first_samples, &first_sample_count, 0u);
    append_i2c_bit(first_samples, &first_sample_count, 0u);
    append_i2c_bit(first_samples, &first_sample_count, 0u);
    append_i2c_bit(first_samples, &first_sample_count, 0u);
    append_i2c_bit(first_samples, &first_sample_count, 0u);
    append_i2c_bit(first_samples, &first_sample_count, 0u);
    append_i2c_bit(first_samples, &first_sample_count, 0u);
    append_i2c_bit(first_samples, &first_sample_count, 1u);
    append_i2c_bit(first_samples, &first_sample_count, 0u);
    append_i2c_bit(first_samples, &first_sample_count, 1u);

    append_i2c_bit(second_samples, &second_sample_count, 1u);
    append_i2c_bit(second_samples, &second_sample_count, 0u);
    append_i2c_bit(second_samples, &second_sample_count, 1u);
    append_i2c_bit(second_samples, &second_sample_count, 0u);
    append_i2c_bit(second_samples, &second_sample_count, 0u);
    append_i2c_sample(second_samples, &second_sample_count, false, false, 2u);
    append_i2c_sample(second_samples, &second_sample_count, false, true, 2u);
    append_i2c_sample(second_samples, &second_sample_count, true, true, 3u);

    first_raw_word_count = pack_i2c_samples(first_raw_words, 8u, first_samples, first_sample_count);
    second_raw_word_count = pack_i2c_samples(second_raw_words, 8u, second_samples, second_sample_count);

    assert(i2c_decoder_process_buffer(
               &decoder_state,
               first_raw_words,
               first_raw_word_count,
               capture_i2c_event_with_reject_after,
               &first_capture
           ) == I2C_DECODER_RESULT_SINK_REJECTED);
    assert(first_capture.capture.count == 1u);
    assert(first_capture.capture.events[0].type == I2C_DECODE_EVENT_START);

    assert(i2c_decoder_process_buffer(
               &decoder_state,
               second_raw_words,
               second_raw_word_count,
               capture_i2c_event,
               &second_capture
           ) == I2C_DECODER_RESULT_OK);
    assert(second_capture.count == 3u);
    assert(second_capture.events[0].type == I2C_DECODE_EVENT_DATA);
    assert(second_capture.events[0].value == 0x5Au);
    assert(second_capture.events[1].type == I2C_DECODE_EVENT_ACK);
    assert(second_capture.events[1].value == 0u);
    assert(second_capture.events[2].type == I2C_DECODE_EVENT_STOP);
}

static void test_i2c_decoder_reset_discards_inflight_transaction_state(void) {
    i2c_decoder_state_t decoder_state;
    uint8_t first_samples[64];
    uint8_t second_samples[128];
    uint32_t first_sample_count = 0u;
    uint32_t second_sample_count = 0u;
    uint32_t first_raw_words[8];
    uint32_t second_raw_words[8];
    uint32_t first_raw_word_count;
    uint32_t second_raw_word_count;
    test_i2c_event_capture_t capture = {0};

    i2c_decoder_init(&decoder_state);

    append_i2c_sample(first_samples, &first_sample_count, true, true, 3u);
    append_i2c_sample(first_samples, &first_sample_count, false, true, 3u);
    append_i2c_bit(first_samples, &first_sample_count, 1u);
    append_i2c_bit(first_samples, &first_sample_count, 0u);
    append_i2c_bit(first_samples, &first_sample_count, 1u);
    append_i2c_bit(first_samples, &first_sample_count, 0u);

    append_i2c_bit(second_samples, &second_sample_count, 0u);
    append_i2c_bit(second_samples, &second_sample_count, 0u);
    append_i2c_bit(second_samples, &second_sample_count, 0u);
    append_i2c_bit(second_samples, &second_sample_count, 0u);
    append_i2c_bit(second_samples, &second_sample_count, 0u);
    append_i2c_bit(second_samples, &second_sample_count, 0u);

    first_raw_word_count = pack_i2c_samples(first_raw_words, 8u, first_samples, first_sample_count);
    second_raw_word_count = pack_i2c_samples(second_raw_words, 8u, second_samples, second_sample_count);

    assert(i2c_decoder_process_buffer(&decoder_state, first_raw_words, first_raw_word_count, capture_i2c_event, &capture) == I2C_DECODER_RESULT_OK);
    assert(capture.count == 1u);
    assert(capture.events[0].type == I2C_DECODE_EVENT_START);

    i2c_decoder_init(&decoder_state);

    assert(i2c_decoder_process_buffer(&decoder_state, second_raw_words, second_raw_word_count, capture_i2c_event, &capture) == I2C_DECODER_RESULT_OK);
    assert(capture.count == 1u);
}

static void test_i2c_decoder_ignores_one_sample_sda_glitch_while_scl_high(void) {
    i2c_decoder_state_t decoder_state;
    uint8_t samples[256];
    uint32_t raw_words[16];
    uint32_t sample_count = 0u;
    uint32_t raw_word_count;
    test_i2c_event_capture_t capture = {0};

    i2c_decoder_init(&decoder_state);

    append_i2c_sample(samples, &sample_count, true, true, 3u);
    append_i2c_sample(samples, &sample_count, false, true, 1u);
    append_i2c_sample(samples, &sample_count, true, true, 3u);

    append_i2c_sample(samples, &sample_count, false, true, 3u);
    append_i2c_bit(samples, &sample_count, 1u);
    append_i2c_bit(samples, &sample_count, 0u);
    append_i2c_bit(samples, &sample_count, 1u);
    append_i2c_bit(samples, &sample_count, 0u);
    append_i2c_bit(samples, &sample_count, 0u);
    append_i2c_bit(samples, &sample_count, 0u);
    append_i2c_bit(samples, &sample_count, 0u);
    append_i2c_bit(samples, &sample_count, 0u);
    append_i2c_bit(samples, &sample_count, 0u);

    append_i2c_bit(samples, &sample_count, 0u);
    append_i2c_bit(samples, &sample_count, 1u);
    append_i2c_bit(samples, &sample_count, 0u);
    append_i2c_bit(samples, &sample_count, 1u);
    append_i2c_bit(samples, &sample_count, 1u);
    append_i2c_bit(samples, &sample_count, 0u);
    append_i2c_bit(samples, &sample_count, 1u);
    append_i2c_bit(samples, &sample_count, 0u);
    append_i2c_bit(samples, &sample_count, 0u);

    append_i2c_sample(samples, &sample_count, false, false, 2u);
    append_i2c_sample(samples, &sample_count, false, true, 2u);
    append_i2c_sample(samples, &sample_count, true, true, 3u);

    raw_word_count = pack_i2c_samples(raw_words, 16u, samples, sample_count);

    assert(i2c_decoder_process_buffer(&decoder_state, raw_words, raw_word_count, capture_i2c_event, &capture) == I2C_DECODER_RESULT_OK);
    assert(capture.count == 6u);
    assert(capture.events[0].type == I2C_DECODE_EVENT_START);
    assert(capture.events[1].type == I2C_DECODE_EVENT_DATA);
    assert(capture.events[1].value == 0xA0u);
    assert(capture.events[2].type == I2C_DECODE_EVENT_ACK);
    assert(capture.events[3].type == I2C_DECODE_EVENT_DATA);
    assert(capture.events[3].value == 0x5Au);
    assert(capture.events[4].type == I2C_DECODE_EVENT_ACK);
    assert(capture.events[5].type == I2C_DECODE_EVENT_STOP);
}

void run_i2c_decoder_tests(void) {
    test_i2c_decoder_emits_events_from_oversampled_buffer();
    test_i2c_decoder_carries_transaction_across_buffers();
    test_i2c_decoder_preserves_progress_after_sink_rejection();
    test_i2c_decoder_reset_discards_inflight_transaction_state();
    test_i2c_decoder_ignores_one_sample_sda_glitch_while_scl_high();
}
