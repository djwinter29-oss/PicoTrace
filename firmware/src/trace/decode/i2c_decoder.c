/**
 * @file i2c_decoder.c
 * @brief Software decoder for oversampled I2C raw buffers.
 *
 * The decoder consumes packed two-pin samples from the I2C monitor ping-pong DMA buffers, detects
 * I2C START and STOP conditions, reconstructs bytes on SCL rising edges, and reports decoded
 * events through a caller-provided callback.
 */

#include "trace/decode/i2c_decoder.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

void i2c_decoder_init(i2c_decoder_state_t *state) {
    if (state == NULL) {
        return;
    }

    memset(state, 0, sizeof(*state));
}

bool i2c_decoder_process_buffer(
    i2c_decoder_state_t *state,
    const uint32_t *raw_words,
    uint32_t raw_word_count,
    i2c_decoder_event_sink_t event_sink,
    void *event_sink_context
) {
    bool have_previous_levels;
    bool previous_sda;
    bool previous_scl;
    bool transaction_active;
    uint8_t bit_count;
    uint8_t current_byte;

    if ((state == NULL) || (raw_words == NULL) || (raw_word_count == 0u)) {
        return false;
    }

    have_previous_levels = state->have_previous_levels;
    previous_sda = state->previous_sda;
    previous_scl = state->previous_scl;
    transaction_active = state->transaction_active;
    bit_count = state->bit_count;
    current_byte = state->current_byte;

    for (uint32_t word_index = 0u; word_index < raw_word_count; ++word_index) {
        uint32_t packed_samples = raw_words[word_index];

        for (uint32_t sample_in_word = 0u; sample_in_word < 16u; ++sample_in_word) {
            uint8_t sample = (uint8_t)((packed_samples >> 30u) & 0x03u);
            bool sda = (sample & 0x01u) != 0u;
            bool scl = (sample & 0x02u) != 0u;

            packed_samples <<= 2u;

            if (!have_previous_levels) {
                have_previous_levels = true;
                previous_sda = sda;
                previous_scl = scl;
                continue;
            }

            if (previous_scl && scl && previous_sda && !sda) {
                transaction_active = true;
                bit_count = 0u;
                current_byte = 0u;
                if ((event_sink != NULL) && !event_sink(event_sink_context, I2C_DECODE_EVENT_START, 0u)) {
                    return false;
                }
            } else if (previous_scl && scl && !previous_sda && sda) {
                transaction_active = false;
                bit_count = 0u;
                current_byte = 0u;
                if ((event_sink != NULL) && !event_sink(event_sink_context, I2C_DECODE_EVENT_STOP, 0u)) {
                    return false;
                }
            }

            if (!previous_scl && scl && transaction_active) {
                if (bit_count < 8u) {
                    current_byte = (uint8_t)((current_byte << 1u) | (sda ? 1u : 0u));
                    bit_count += 1u;
                    if (bit_count == 8u) {
                        if ((event_sink != NULL) && !event_sink(event_sink_context, I2C_DECODE_EVENT_DATA, current_byte)) {
                            return false;
                        }
                    }
                } else {
                    if ((event_sink != NULL) && !event_sink(event_sink_context, I2C_DECODE_EVENT_ACK, sda ? 1u : 0u)) {
                        return false;
                    }
                    bit_count = 0u;
                    current_byte = 0u;
                }
            }

            previous_sda = sda;
            previous_scl = scl;
        }
    }

    state->have_previous_levels = have_previous_levels;
    state->previous_sda = previous_sda;
    state->previous_scl = previous_scl;
    state->transaction_active = transaction_active;
    state->bit_count = bit_count;
    state->current_byte = current_byte;
    return true;
}