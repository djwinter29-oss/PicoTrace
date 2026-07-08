/**
 * @file device_cli.c
 * @brief Board-local CLI commands exposed over the USB CDC shell.
 */

#include "cli/device_cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_control.h"
#include "trace/capture/i2c_monitor_control.h"
#include "trace/capture/spi_monitor_control.h"

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
/** @brief Handle unknown command names by printing fixed help. */
static bool device_cli_unknown(const char *command_name);

/** @brief Static command table registered with the CLI shell. */
static const cli_shell_command_t device_cli_commands[] = {
    {"help", "help", device_cli_help},
    {"i2cmon", "i2cmon <channel> <sample_hz>|status <channel>", device_cli_i2cmon},
    {"led", "led on|off", device_cli_led},
    {"stream", "stream on|off", device_cli_stream},
    {"reboot", "reboot", device_cli_reboot},
    {"spimon", "spimon <bus> off|mosi|both|status", device_cli_spimon},
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

    snprintf(
        line,
        sizeof(line),
        "i2cmon ch%lu %s hz=%lu buffers=%lu overruns=%lu sticky=%u pending=%u reason=%u",
        (unsigned long)channel,
        status->running ? "running" : "stopped",
        (unsigned long)status->sample_hz,
        (unsigned long)status->completed_buffers,
        (unsigned long)status->overrun_count,
        status->overrun ? 1u : 0u,
        status->transition_pending ? 1u : 0u,
        (unsigned int)status->transition_reason
    );
    return cli_shell_write_line(line);
}

/** @brief Map the SPI capture selection enum to a CLI-visible token. */
static const char *device_cli_spi_capture_name(spi_monitor_capture_t capture) {
    switch (capture) {
        case SPI_MONITOR_CAPTURE_MOSI:
            return "mosi";
        case SPI_MONITOR_CAPTURE_MOSI_MISO:
            return "both";
        case SPI_MONITOR_CAPTURE_DISABLED:
        default:
            return "off";
    }
}

/** @brief Map the selected `CS_N` mask to a CLI-visible token. */
static const char *device_cli_spi_channel_select_name(uint8_t channel_select_mask) {
    switch (channel_select_mask) {
        case 0x01u:
            return "ch0";
        case 0x02u:
            return "ch1";
        case 0x04u:
            return "ch2";
        case SPI_MONITOR_CHANNEL_SELECT_ALL:
            return "all";
        default:
            return "mixed";
    }
}

/** @brief Format one SPI monitor bus status line for the CLI. */
static bool device_cli_write_spimon_status(uint32_t bus, const spi_monitor_bus_status_t *status) {
    char line[128];

    snprintf(
        line,
        sizeof(line),
        "spimon bus%lu %s select=%s capture=%s mode=%u timeout_us=%lu packets=%lu overruns=%lu",
        (unsigned long)bus,
        status->running ? "running" : "stopped",
        device_cli_spi_channel_select_name(status->channel_select_mask),
        device_cli_spi_capture_name(status->capture),
        (unsigned int)status->spi_mode,
        (unsigned long)status->timeout_us,
        (unsigned long)status->packets_emitted,
        (unsigned long)status->overrun_count
    );
    return cli_shell_write_line(line);
}

/** @brief Emit the fixed help listing for all registered CLI commands. */
static bool device_cli_write_help(void) {
    cli_shell_write_line("Commands:");
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
    i2c_monitor_rc_t result;
    uint32_t channel;
    uint32_t sample_hz;

    if (argc == 3) {
        if ((strcmp(argv[1], "status") == 0) && device_cli_parse_u32(argv[2], &channel)) {
            if (i2c_monitor_control_get_channel_status(channel, &status) != I2C_MONITOR_RC_OK) {
                return cli_shell_write_line("i2cmon status failed");
            }

            return device_cli_write_i2cmon_status(channel, &status);
        }

        if (device_cli_parse_u32(argv[1], &channel) && device_cli_parse_u32(argv[2], &sample_hz)) {
            result = i2c_monitor_control_set_channel_sample_hz(channel, sample_hz);
            if (result != I2C_MONITOR_RC_OK) {
                if (result == I2C_MONITOR_RC_BUSY) {
                    return cli_shell_write_line("i2cmon busy");
                }
                if (result == I2C_MONITOR_RC_DISABLED) {
                    return cli_shell_write_line("i2cmon disabled");
                }
                return cli_shell_write_line("i2cmon apply failed");
            }

            if (i2c_monitor_control_get_channel_status(channel, &status) != I2C_MONITOR_RC_OK) {
                return cli_shell_write_line("i2cmon status failed");
            }

            return device_cli_write_i2cmon_status(channel, &status);
        }
    }

    return cli_shell_write_line("Usage: i2cmon <channel> <sample_hz>|status <channel>");
}

/** @copydoc device_cli_spimon */
static bool device_cli_spimon(int argc, const char *const *argv) {
    spi_monitor_bus_status_t status;
    spi_monitor_bus_config_t config;
    spi_monitor_rc_t result;
    uint32_t bus;
    uint32_t channel_slot;
    uint32_t spi_mode;
    uint32_t timeout_us = 0u;

    if ((argc == 3) && (strcmp(argv[1], "status") == 0) && device_cli_parse_u32(argv[2], &bus)) {
        if (spi_monitor_control_get_bus_status(bus, &status) != SPI_MONITOR_RC_OK) {
            return cli_shell_write_line("spimon status failed");
        }

        return device_cli_write_spimon_status(bus, &status);
    }

    if ((argc == 3) && device_cli_parse_u32(argv[1], &bus) && (strcmp(argv[2], "off") == 0)) {
        memset(&config, 0, sizeof(config));
        config.capture = SPI_MONITOR_CAPTURE_DISABLED;
        result = spi_monitor_control_set_bus_config(bus, &config);
        if (result != SPI_MONITOR_RC_OK) {
            if (result == SPI_MONITOR_RC_BUSY) {
                return cli_shell_write_line("spimon busy");
            }
            if (result == SPI_MONITOR_RC_DISABLED) {
                return cli_shell_write_line("spimon disabled");
            }
            return cli_shell_write_line("spimon apply failed");
        }

        if (spi_monitor_control_get_bus_status(bus, &status) != SPI_MONITOR_RC_OK) {
            return cli_shell_write_line("spimon status failed");
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
            return cli_shell_write_line("Usage: spimon <bus> off|mosi <all|channel> <mode> [timeout_us]|both <all|channel> <mode> [timeout_us]|status <bus>");
        }

        if (strcmp(argv[3], "all") == 0) {
            config.channel_select_mask = SPI_MONITOR_CHANNEL_SELECT_ALL;
        } else if (device_cli_parse_u32(argv[3], &channel_slot) && (channel_slot < SPI_MONITOR_CS_SLOTS_PER_BUS)) {
            config.channel_select_mask = (uint8_t)(1u << channel_slot);
        } else {
            return cli_shell_write_line("Usage: spimon <bus> off|mosi <all|channel> <mode> [timeout_us]|both <all|channel> <mode> [timeout_us]|status <bus>");
        }

        if ((argc == 6) && !device_cli_parse_u32(argv[5], &timeout_us)) {
            return cli_shell_write_line("Usage: spimon <bus> off|mosi <all|channel> <mode> [timeout_us]|both <all|channel> <mode> [timeout_us]|status <bus>");
        }

        config.spi_mode = (uint8_t)spi_mode;
        config.timeout_us = timeout_us;
        result = spi_monitor_control_set_bus_config(bus, &config);
        if (result != SPI_MONITOR_RC_OK) {
            if (result == SPI_MONITOR_RC_BUSY) {
                return cli_shell_write_line("spimon busy");
            }
            if (result == SPI_MONITOR_RC_DISABLED) {
                return cli_shell_write_line("spimon disabled");
            }
            return cli_shell_write_line("spimon apply failed");
        }

        if (spi_monitor_control_get_bus_status(bus, &status) != SPI_MONITOR_RC_OK) {
            return cli_shell_write_line("spimon status failed");
        }

        return device_cli_write_spimon_status(bus, &status);
    }

    return cli_shell_write_line("Usage: spimon <bus> off|mosi <all|channel> <mode> [timeout_us]|both <all|channel> <mode> [timeout_us]|status <bus>");
}

/** @copydoc device_cli_led */
static bool device_cli_led(int argc, const char *const *argv) {
    if (argc != 2) {
        return cli_shell_write_line("Usage: led on|off");
    }

    if (strcmp(argv[1], "on") == 0) {
        app_control_set_led(true);
        return cli_shell_write_line("LED on");
    }

    if (strcmp(argv[1], "off") == 0) {
        app_control_set_led(false);
        return cli_shell_write_line("LED off");
    }

    return cli_shell_write_line("Usage: led on|off");
}

/** @copydoc device_cli_stream */
static bool device_cli_stream(int argc, const char *const *argv) {
    if (argc != 2) {
        return cli_shell_write_line("Usage: stream on|off");
    }

    if (strcmp(argv[1], "on") == 0) {
        app_control_set_stream_enabled(true);
        return cli_shell_write_line("Stream on");
    }

    if (strcmp(argv[1], "off") == 0) {
        app_control_set_stream_enabled(false);
        return cli_shell_write_line("Stream off");
    }

    return cli_shell_write_line("Usage: stream on|off");
}

/** @copydoc device_cli_reboot */
static bool device_cli_reboot(int argc, const char *const *argv) {
    (void)argv;

    if (argc != 1) {
        return cli_shell_write_line("Usage: reboot");
    }

    app_control_reboot();
    return true;
}

/** @copydoc device_cli_unknown */
static bool device_cli_unknown(const char *command_name) {
    (void)command_name;

    cli_shell_write_line("Unknown command.");
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
        .unknown_message = "Unknown command. Type help.",
        .unknown_handler = device_cli_unknown,
    };

    cli_shell_init(&config);
}

void device_cli_poll(void) {
    cli_shell_poll();
}