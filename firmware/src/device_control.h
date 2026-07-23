/**
 * @file device_control.h
 * @brief Transport-neutral helpers for shared device, I2C, and SPI control actions.
 */

#ifndef DEVICE_CONTROL_H
#define DEVICE_CONTROL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "trace/capture/i2c/i2c_monitor_control.h"
#include "trace/capture/spi/spi_monitor_control.h"

#define DEVICE_CONTROL_DEVICE_STATUS_FIXED_BYTES 2u
#define DEVICE_CONTROL_DEVICE_STATUS_MAX_VERSION_BYTES 58u
#define DEVICE_CONTROL_I2C_MONITOR_STATUS_PAYLOAD_BYTES 18u
#define DEVICE_CONTROL_I2C_MONITOR_ALL_STATUS_CHANNEL_BYTES 14u
#define DEVICE_CONTROL_I2C_MONITOR_ALL_STATUS_PAYLOAD_BYTES (I2C_MONITOR_CHANNEL_COUNT * DEVICE_CONTROL_I2C_MONITOR_ALL_STATUS_CHANNEL_BYTES)
#define DEVICE_CONTROL_SPI_MONITOR_STATUS_PAYLOAD_BYTES 46u
#define DEVICE_CONTROL_SPI_MONITOR_ALL_STATUS_CHANNEL_BYTES 10u
#define DEVICE_CONTROL_SPI_MONITOR_ALL_STATUS_PAYLOAD_BYTES (SPI_MONITOR_CHANNEL_COUNT * DEVICE_CONTROL_SPI_MONITOR_ALL_STATUS_CHANNEL_BYTES)

/** @brief Normalized shared control results used by CDC and HID adapters. */
typedef enum {
    DEVICE_CONTROL_RESULT_OK = 0u, /**< Operation completed successfully. */
    DEVICE_CONTROL_RESULT_BUSY = 1u, /**< Operation could not run because the target was busy. */
    DEVICE_CONTROL_RESULT_DISABLED = 2u, /**< Operation was rejected because streaming or control was disabled. */
    DEVICE_CONTROL_RESULT_INVALID = 3u, /**< Operation input was invalid. */
    DEVICE_CONTROL_RESULT_REJECTED = 4u, /**< Operation was otherwise rejected. */
} device_control_result_t;

/** @brief Shared device status fields exposed over CDC and HID control paths. */
typedef struct {
    bool stream_enabled; /**< Current vendor bulk stream enable state. */
    const char *firmware_version; /**< Build-time firmware version string. */
} device_control_status_t;

/** @brief Shared CLI help text for the `help` command. */
extern const char device_control_cli_help_command_help[];

/** @brief Shared CLI help text for the `i2cmon` command. */
extern const char device_control_cli_i2cmon_command_help[];

/** @brief Shared CLI help text for the `led` command. */
extern const char device_control_cli_led_command_help[];

/** @brief Shared CLI help text for the `stream` command. */
extern const char device_control_cli_stream_command_help[];

/** @brief Shared CLI help text for the `reboot` command. */
extern const char device_control_cli_reboot_command_help[];

/** @brief Shared CLI help text for the `spimon` command. */
extern const char device_control_cli_spimon_command_help[];

/** @brief Shared CLI help text for the `version` command. */
extern const char device_control_cli_version_command_help[];

/** @brief Shared CLI header emitted before the command listing. */
extern const char device_control_cli_commands_header[];

/** @brief Shared CLI usage line for `spimon` parse failures. */
extern const char device_control_cli_spimon_usage_line[];

/** @brief Shared CLI line emitted by the explicit unknown-command handler. */
extern const char device_control_cli_unknown_line[];

/** @brief Shared CLI shell message emitted for unknown commands. */
extern const char device_control_cli_unknown_message[];

/** @brief Return shared device status used by multiple control transports. */
device_control_status_t device_control_get_status(void);

/** @brief Return the fixed-format byte length of the shared binary device status payload. */
uint8_t device_control_get_device_status_payload_length(void);

/** @brief Encode the shared binary device status payload used by HID. */
uint8_t device_control_encode_device_status_payload(uint8_t *payload, uint32_t capacity);

/** @brief Map one normalized shared control result to the HID status byte value. */
uint8_t device_control_result_to_hid_status(device_control_result_t result);

/** @brief Return the CLI-visible error line for one I2C apply result. */
const char *device_control_i2c_apply_error_line(device_control_result_t result);

/** @brief Return the CLI-visible error line for one SPI apply result. */
const char *device_control_spi_apply_error_line(device_control_result_t result);

/** @brief Format the shared firmware-version CLI line into a caller-owned buffer. */
bool device_control_format_version_line(char *buffer, size_t capacity);

/** @brief Return the CLI-visible success line after turning the LED on. */
const char *device_control_led_on_line(void);

/** @brief Return the CLI-visible success line after turning the LED off. */
const char *device_control_led_off_line(void);

/** @brief Return the CLI-visible success line after enabling streaming. */
const char *device_control_stream_on_line(void);

/** @brief Return the CLI-visible success line after disabling streaming. */
const char *device_control_stream_off_line(void);

/** @brief Update the shared vendor bulk stream enable state. */
void device_control_set_stream_enabled(bool enabled);

/** @brief Update the shared board LED state. */
void device_control_set_led(bool on);

/** @brief Request a board reboot through the shared control path. */
void device_control_reboot(void);

/** @brief Start, stop, or retune one I2C monitor channel. */
i2c_monitor_rc_t device_control_i2c_set_channel_sample_hz(uint32_t channel, uint32_t sample_hz);

/** @brief Start, stop, or retune one I2C monitor channel and return the resulting status snapshot. */
device_control_result_t device_control_i2c_configure_channel(uint32_t channel, uint32_t sample_hz, i2c_monitor_channel_status_t *status_out);

/** @brief Read one I2C monitor channel status snapshot. */
i2c_monitor_rc_t device_control_i2c_get_channel_status(uint32_t channel, i2c_monitor_channel_status_t *status_out);

/** @brief Read one I2C monitor channel status snapshot using the shared normalized result. */
device_control_result_t device_control_i2c_read_channel_status(uint32_t channel, i2c_monitor_channel_status_t *status_out);

/** @brief Read all I2C monitor channel status snapshots. */
i2c_monitor_rc_t device_control_i2c_get_all_status(i2c_monitor_channel_status_t *status_out);

/** @brief Read all I2C monitor channel status snapshots using the shared normalized result. */
device_control_result_t device_control_i2c_read_all_status(i2c_monitor_channel_status_t *status_out);

/** @brief Encode one I2C monitor channel status snapshot into the shared binary payload format. */
uint8_t device_control_encode_i2c_channel_status_payload(uint32_t channel, const i2c_monitor_channel_status_t *status, uint8_t *payload, uint32_t capacity);

/** @brief Format one I2C monitor channel status snapshot into the shared CLI line format. */
bool device_control_format_i2c_channel_status_line(uint32_t channel, const i2c_monitor_channel_status_t *status, char *buffer, size_t capacity);

/** @brief Encode all I2C monitor channel status snapshots into the shared compact binary payload format. */
uint8_t device_control_encode_i2c_all_status_payload(const i2c_monitor_channel_status_t *status, uint32_t status_count, uint8_t *payload, uint32_t capacity);

/** @brief Start, stop, or reconfigure one observed SPI bus. */
spi_monitor_rc_t device_control_spi_set_bus_config(uint32_t bus, const spi_monitor_bus_config_t *config);

/** @brief Start, stop, or reconfigure one observed SPI bus and return the resulting status snapshot. */
device_control_result_t device_control_spi_configure_bus(uint32_t bus, const spi_monitor_bus_config_t *config, spi_monitor_bus_status_t *status_out);

/** @brief Read one observed SPI bus status snapshot. */
spi_monitor_rc_t device_control_spi_get_bus_status(uint32_t bus, spi_monitor_bus_status_t *status_out);

/** @brief Read one observed SPI bus status snapshot using the shared normalized result. */
device_control_result_t device_control_spi_read_bus_status(uint32_t bus, spi_monitor_bus_status_t *status_out);

/** @brief Read all SPI monitor channel status snapshots. */
spi_monitor_rc_t device_control_spi_get_all_status(spi_monitor_channel_status_t *status_out);

/** @brief Read all SPI monitor channel status snapshots using the shared normalized result. */
device_control_result_t device_control_spi_read_all_status(spi_monitor_channel_status_t *status_out);

/** @brief Encode one observed SPI bus status snapshot into the shared binary payload format. */
uint8_t device_control_encode_spi_bus_status_payload(uint32_t bus, const spi_monitor_bus_status_t *status, uint8_t *payload, uint32_t capacity);

/** @brief Format one observed SPI bus status snapshot into the shared CLI line format. */
bool device_control_format_spi_bus_status_line(uint32_t bus, const spi_monitor_bus_status_t *status, char *buffer, size_t capacity);

/** @brief Encode all SPI monitor channel status snapshots into the shared compact binary payload format. */
uint8_t device_control_encode_spi_all_status_payload(const spi_monitor_channel_status_t *status, uint32_t status_count, uint8_t *payload, uint32_t capacity);

#endif