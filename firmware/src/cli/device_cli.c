/**
 * @file device_cli.c
 * @brief Board-local CLI commands exposed over the USB CDC shell.
 */

#include "cli/device_cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device_control.h"

/** @brief Write the CLI help text. */
static bool device_cli_help(int argc, const char *const *argv);
/** @brief Handle `i2cmon` control and status commands. */
static bool device_cli_i2cmon(int argc, const char *const *argv);
/** @brief Handle `spimon` control and status commands. */
static bool device_cli_spimon(int argc, const char *const *argv);
/** @brief Handle board LED control commands. */
static bool device_cli_led(int argc, const char *const *argv);
/** @brief Handle shared stream enable and disable commands. */
static bool device_cli_stream(int argc, const char *const *argv);
/** @brief Handle the reboot command. */
static bool device_cli_reboot(int argc, const char *const *argv);
/** @brief Handle the firmware version command. */
static bool device_cli_version(int argc, const char *const *argv);
/** @brief Handle unknown command names by printing fixed help. */
static bool device_cli_unknown(const char *command_name);

/** @brief Static command table registered with the CLI shell. */
static const cli_shell_command_t device_cli_commands[] = {
    {"help", device_control_cli_help_command_help, device_cli_help},
    {"i2cmon", device_control_cli_i2cmon_command_help, device_cli_i2cmon},
    {"led", device_control_cli_led_command_help, device_cli_led},
    {"stream", device_control_cli_stream_command_help, device_cli_stream},
    {"reboot", device_control_cli_reboot_command_help, device_cli_reboot},
    {"spimon", device_control_cli_spimon_command_help, device_cli_spimon},
    {"version", device_control_cli_version_command_help, device_cli_version},
};

/**
 * @brief Parse a decimal `uint32_t` argument.
 * @param text Null-terminated decimal text.
 * @param value_out Caller-owned destination for the parsed value.
 * @return `true` when parsing succeeded, otherwise `false`.
 */
static bool device_cli_parse_u32(const char *text, uint32_t *value_out) {
    char *end = NULL;
    unsigned long parsed;

    if ((text == NULL) || (value_out == NULL) || (text[0] == '\0')) {
        return false;
    }

    parsed = strtoul(text, &end, 10);
    if ((end == text) || (end == NULL) || (*end != '\0')) {
        return false;
    }

    *value_out = (uint32_t)parsed;
    return true;
}

/** @brief Format one I2C monitor status line for the CLI. */
static bool device_cli_write_i2cmon_status(uint32_t channel, const i2c_monitor_channel_status_t *status) {
    char line[96];

    if (!device_control_format_i2c_channel_status_line(channel, status, line, sizeof(line))) {
        return false;
    }

    return cli_shell_write_line(line);
}

/** @brief Format one SPI monitor bus status line for the CLI. */
static bool device_cli_write_spimon_status(uint32_t bus, const spi_monitor_bus_status_t *status) {
    char line[128];

    if (!device_control_format_spi_bus_status_line(bus, status, line, sizeof(line))) {
        return false;
    }

    return cli_shell_write_line(line);
}

/** @brief Emit the fixed help listing for all registered CLI commands. */
static bool device_cli_write_help(void) {
    cli_shell_write_line(device_control_cli_commands_header);
    for (uint32_t index = 0u; index < (sizeof(device_cli_commands) / sizeof(device_cli_commands[0])); ++index) {
        cli_shell_write_line(device_cli_commands[index].help);
    }

    return true;
}

/** @copydoc device_cli_help */
static bool device_cli_help(int argc, const char *const *argv) {
    (void)argc;
    (void)argv;

    return device_cli_write_help();
}

/** @copydoc device_cli_i2cmon */
static bool device_cli_i2cmon(int argc, const char *const *argv) {
    i2c_monitor_channel_status_t status;
    device_control_result_t result;
    uint32_t channel;
    uint32_t sample_hz;

    if (argc == 3) {
        if ((strcmp(argv[1], "status") == 0) && device_cli_parse_u32(argv[2], &channel)) {
            if (device_control_i2c_read_channel_status(channel, &status) != DEVICE_CONTROL_RESULT_OK) {
                return cli_shell_write_line("i2cmon status failed");
            }

            return device_cli_write_i2cmon_status(channel, &status);
        }

        if (device_cli_parse_u32(argv[1], &channel) && device_cli_parse_u32(argv[2], &sample_hz)) {
            result = device_control_i2c_configure_channel(channel, sample_hz, &status);
            if (result != DEVICE_CONTROL_RESULT_OK) {
                return cli_shell_write_line(device_control_i2c_apply_error_line(result));
            }

            return device_cli_write_i2cmon_status(channel, &status);
        }
    }

    return cli_shell_write_line(device_control_cli_i2cmon_command_help);
}

/** @copydoc device_cli_spimon */
static bool device_cli_spimon(int argc, const char *const *argv) {
    spi_monitor_bus_status_t status;
    spi_monitor_bus_config_t config;
    device_control_result_t result;
    uint32_t bus;
    uint32_t channel_slot;
    uint32_t spi_mode;
    uint32_t timeout_us = 0u;

    if ((argc == 3) && (strcmp(argv[1], "status") == 0) && device_cli_parse_u32(argv[2], &bus)) {
        if (device_control_spi_read_bus_status(bus, &status) != DEVICE_CONTROL_RESULT_OK) {
            return cli_shell_write_line("spimon status failed");
        }

        return device_cli_write_spimon_status(bus, &status);
    }

    if ((argc == 3) && device_cli_parse_u32(argv[1], &bus) && (strcmp(argv[2], "off") == 0)) {
        memset(&config, 0, sizeof(config));
        config.capture = SPI_MONITOR_CAPTURE_DISABLED;
        result = device_control_spi_configure_bus(bus, &config, &status);
        if (result != DEVICE_CONTROL_RESULT_OK) {
            return cli_shell_write_line(device_control_spi_apply_error_line(result));
        }

        return device_cli_write_spimon_status(bus, &status);
    }

    if ((argc == 5 || argc == 6) && device_cli_parse_u32(argv[1], &bus) && device_cli_parse_u32(argv[4], &spi_mode)) {
        memset(&config, 0, sizeof(config));
        if (strcmp(argv[2], "mosi") == 0) {
            config.capture = SPI_MONITOR_CAPTURE_MOSI;
        } else if (strcmp(argv[2], "both") == 0) {
            config.capture = SPI_MONITOR_CAPTURE_MOSI_MISO;
        } else {
            return cli_shell_write_line(device_control_cli_spimon_usage_line);
        }

        if (strcmp(argv[3], "all") == 0) {
            config.channel_select_mask = SPI_MONITOR_CHANNEL_SELECT_ALL;
        } else if (device_cli_parse_u32(argv[3], &channel_slot) && (channel_slot < SPI_MONITOR_CS_SLOTS_PER_BUS)) {
            config.channel_select_mask = (uint8_t)(1u << channel_slot);
        } else {
            return cli_shell_write_line(device_control_cli_spimon_usage_line);
        }

        if ((argc == 6) && !device_cli_parse_u32(argv[5], &timeout_us)) {
            return cli_shell_write_line(device_control_cli_spimon_usage_line);
        }

        config.spi_mode = (uint8_t)spi_mode;
        config.timeout_us = timeout_us;
        result = device_control_spi_configure_bus(bus, &config, &status);
        if (result != DEVICE_CONTROL_RESULT_OK) {
            return cli_shell_write_line(device_control_spi_apply_error_line(result));
        }

        return device_cli_write_spimon_status(bus, &status);
    }

    return cli_shell_write_line(device_control_cli_spimon_usage_line);
}

/** @copydoc device_cli_led */
static bool device_cli_led(int argc, const char *const *argv) {
    if (argc != 2) {
        return cli_shell_write_line(device_control_cli_led_command_help);
    }

    if (strcmp(argv[1], "on") == 0) {
        device_control_set_led(true);
        return cli_shell_write_line(device_control_led_on_line());
    }

    if (strcmp(argv[1], "off") == 0) {
        device_control_set_led(false);
        return cli_shell_write_line(device_control_led_off_line());
    }

    return cli_shell_write_line(device_control_cli_led_command_help);
}

/** @copydoc device_cli_stream */
static bool device_cli_stream(int argc, const char *const *argv) {
    if (argc != 2) {
        return cli_shell_write_line(device_control_cli_stream_command_help);
    }

    if (strcmp(argv[1], "on") == 0) {
        device_control_set_stream_enabled(true);
        return cli_shell_write_line(device_control_stream_on_line());
    }

    if (strcmp(argv[1], "off") == 0) {
        device_control_set_stream_enabled(false);
        return cli_shell_write_line(device_control_stream_off_line());
    }

    return cli_shell_write_line(device_control_cli_stream_command_help);
}

/** @copydoc device_cli_reboot */
static bool device_cli_reboot(int argc, const char *const *argv) {
    (void)argv;

    if (argc != 1) {
        return cli_shell_write_line(device_control_cli_reboot_command_help);
    }

    device_control_reboot();
    return true;
}

/** @copydoc device_cli_version */
static bool device_cli_version(int argc, const char *const *argv) {
    char line[80];

    (void)argv;

    if (argc != 1) {
        return cli_shell_write_line(device_control_cli_version_command_help);
    }

    if (!device_control_format_version_line(line, sizeof(line))) {
        return false;
    }

    return cli_shell_write_line(line);
}

/** @copydoc device_cli_unknown */
static bool device_cli_unknown(const char *command_name) {
    (void)command_name;

    cli_shell_write_line(device_control_cli_unknown_line);
    return device_cli_write_help();
}

/**
 * @brief Initialize the device CLI with the supplied shell transport.
 * @param transport Caller-owned shell transport binding.
 */
void device_cli_init(const cli_shell_transport_t *transport) {
    cli_shell_config_t config = {
        .transport = transport,
        .commands = device_cli_commands,
        .command_count = sizeof(device_cli_commands) / sizeof(device_cli_commands[0]),
        .unknown_message = device_control_cli_unknown_message,
        .unknown_handler = device_cli_unknown,
    };

    cli_shell_init(&config);
}

void device_cli_poll(void) {
    cli_shell_poll();
}