#include "cli/device_cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_control.h"
#include "trace/capture/i2c_monitor_control.h"

static bool device_cli_help(int argc, const char *const *argv);
static bool device_cli_i2cmon(int argc, const char *const *argv);
static bool device_cli_led(int argc, const char *const *argv);
static bool device_cli_stream(int argc, const char *const *argv);
static bool device_cli_reboot(int argc, const char *const *argv);
static bool device_cli_unknown(const char *command_name);

static const cli_shell_command_t device_cli_commands[] = {
    {"help", "help", device_cli_help},
    {"i2cmon", "i2cmon <channel> <sample_hz>|status <channel>", device_cli_i2cmon},
    {"led", "led on|off", device_cli_led},
    {"stream", "stream on|off", device_cli_stream},
    {"reboot", "reboot", device_cli_reboot},
};

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

static bool device_cli_write_i2cmon_status(uint32_t channel, const i2c_monitor_channel_status_t *status) {
    char line[96];

    snprintf(
        line,
        sizeof(line),
        "i2cmon ch%lu %s hz=%lu buffers=%lu overruns=%lu sticky=%u",
        (unsigned long)channel,
        status->running ? "running" : "stopped",
        (unsigned long)status->sample_hz,
        (unsigned long)status->completed_buffers,
        (unsigned long)status->overrun_count,
        status->overrun ? 1u : 0u
    );
    return cli_shell_write_line(line);
}

static bool device_cli_write_help(void) {
    cli_shell_write_line("Commands:");
    for (uint32_t index = 0u; index < (sizeof(device_cli_commands) / sizeof(device_cli_commands[0])); ++index) {
        cli_shell_write_line(device_cli_commands[index].help);
    }

    return true;
}

static bool device_cli_help(int argc, const char *const *argv) {
    (void)argc;
    (void)argv;

    return device_cli_write_help();
}

static bool device_cli_i2cmon(int argc, const char *const *argv) {
    i2c_monitor_channel_status_t status;
    uint32_t channel;
    uint32_t sample_hz;

    if (argc == 3) {
        if ((strcmp(argv[1], "status") == 0) && device_cli_parse_u32(argv[2], &channel)) {
            if (!i2c_monitor_control_get_channel_status(channel, &status)) {
                return cli_shell_write_line("i2cmon status failed");
            }

            return device_cli_write_i2cmon_status(channel, &status);
        }

        if (device_cli_parse_u32(argv[1], &channel) && device_cli_parse_u32(argv[2], &sample_hz)) {
            if (!i2c_monitor_control_set_channel_sample_hz(channel, sample_hz)) {
                return cli_shell_write_line("i2cmon apply failed");
            }

            if (!i2c_monitor_control_get_channel_status(channel, &status)) {
                return cli_shell_write_line("i2cmon status failed");
            }

            return device_cli_write_i2cmon_status(channel, &status);
        }
    }

    return cli_shell_write_line("Usage: i2cmon <channel> <sample_hz>|status <channel>");
}

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

static bool device_cli_reboot(int argc, const char *const *argv) {
    (void)argv;

    if (argc != 1) {
        return cli_shell_write_line("Usage: reboot");
    }

    app_control_reboot();
    return true;
}

static bool device_cli_unknown(const char *command_name) {
    (void)command_name;

    cli_shell_write_line("Unknown command.");
    return device_cli_write_help();
}

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