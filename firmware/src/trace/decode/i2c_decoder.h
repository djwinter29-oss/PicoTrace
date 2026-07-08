/**
 * @file i2c_decoder.h
 * @brief Decoder for oversampled I2C raw buffers captured by the sampler ping-pong DMA path.
 */

#ifndef I2C_DECODER_H
#define I2C_DECODER_H

#include <stdbool.h>
#include <stdint.h>

/** @brief Decoded I2C event types emitted from one oversampled raw buffer. */
typedef enum {
    I2C_DECODE_EVENT_START = 1u, /**< I2C START or repeated START condition. */
    I2C_DECODE_EVENT_DATA = 2u, /**< Reconstructed 8-bit I2C data byte. */
    I2C_DECODE_EVENT_ACK = 3u, /**< ACK or NACK bit sampled after one data byte. */
    I2C_DECODE_EVENT_STOP = 4u, /**< I2C STOP condition. */
} i2c_decode_event_type_t;

/** @brief One decoded I2C event stored in a trace packet payload. */
typedef struct {
    uint8_t type; /**< Event type encoded as @ref i2c_decode_event_type_t. */
    uint8_t value; /**< Event payload value, such as a byte value or ACK bit state. */
} i2c_decode_event_t;

/** @brief Caller-owned decode state carried across consecutive ping-pong buffers. */
typedef struct {
    bool have_previous_levels; /**< Indicates whether the decoder already has one previous sampled SDA/SCL level pair. */
    bool previous_sda; /**< Previously observed SDA level for edge detection across sample boundaries. */
    bool previous_scl; /**< Previously observed SCL level for edge detection across sample boundaries. */
    bool transaction_active; /**< Indicates whether the decoder is currently inside an active I2C transaction. */
    uint8_t bit_count; /**< Number of data bits already shifted into @ref current_byte for the active byte. */
    uint8_t current_byte; /**< Byte under construction from oversampled SCL rising-edge captures. */
} i2c_decoder_state_t;

/**
 * @brief Event sink used by the decoder when a decoded I2C event is ready.
 * @param context Caller-owned callback context.
 * @param event_type Event type encoded as @ref i2c_decode_event_type_t.
 * @param event_value Event payload value, such as a byte value or ACK bit state.
 * @return `true` to continue decoding, or `false` to stop because the caller could not accept the
 * event.
 */
typedef bool (*i2c_decoder_event_sink_t)(void *context, uint8_t event_type, uint8_t event_value);

/**
 * @brief Reset one decoder instance to its initial empty state.
 * @param state Caller-owned decode state to initialize.
 */
void i2c_decoder_init(i2c_decoder_state_t *state);

/**
 * @brief Walk one completed oversampled raw buffer and report decoded I2C events.
 * @param state Caller-owned decode state carried across buffers.
 * @param raw_words Packed 32-bit raw sample words, each containing 16 two-bit SDA/SCL samples.
 * @param raw_word_count Number of valid entries in @p raw_words.
 * @param event_sink Callback invoked for each decoded event. Pass `NULL` to advance state without
 * emitting any decoded output.
 * @param event_sink_context Caller-owned callback context passed back to @p event_sink.
 * @return `true` when decoding completed successfully, or `false` if the input was invalid or the
 * event sink rejected one of the decoded events.
 */
bool i2c_decoder_process_buffer(
    i2c_decoder_state_t *state,
    const uint32_t *raw_words,
    uint32_t raw_word_count,
    i2c_decoder_event_sink_t event_sink,
    void *event_sink_context
);

#endif