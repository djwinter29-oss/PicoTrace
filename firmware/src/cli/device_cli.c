#include "cli/device_cli.h"

#include <string.h>

#include "app_control.h"

static bool device_cli_help(int argc, const char *const *argv);
static bool device_cli_led(int argc, const char *const *argv);
static bool device_cli_stream(int argc, const char *const *argv);
static bool device_cli_reboot(int argc, const char *const *argv);
static bool device_cli_unknown(const char *command_name);

static const cli_shell_command_t device_cli_commands[] = {
    {"help", "help", device_cli_help},
    {"led", "led on|off", device_cli_led},
    {"stream", "stream on|off", device_cli_stream},
    {"reboot", "reboot", device_cli_reboot},
};

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