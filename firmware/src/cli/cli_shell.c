#include "cli/cli_shell.h"

#include <string.h>

#define CLI_SHELL_LINE_SIZE 64u
#define CLI_SHELL_ARG_COUNT 8u
#define CLI_SHELL_RX_CHUNK_SIZE 16u

typedef struct {
    cli_shell_transport_t transport;
    const cli_shell_command_t *commands;
    uint32_t command_count;
    const char *unknown_message;
    cli_shell_unknown_handler_t unknown_handler;
    char line[CLI_SHELL_LINE_SIZE];
    uint32_t line_length;
    bool initialized;
} cli_shell_state_t;

static cli_shell_state_t cli_shell_state;

static bool cli_shell_write_bytes(const uint8_t *data, uint32_t length) {
    if ((cli_shell_state.transport.write == NULL) || (data == NULL)) {
        return false;
    }

    if (length == 0u) {
        return true;
    }

    return cli_shell_state.transport.write(cli_shell_state.transport.context, data, length);
}

bool cli_shell_write(const char *text) {
    if (text == NULL) {
        return false;
    }

    return cli_shell_write_bytes((const uint8_t *)text, (uint32_t)strlen(text));
}

bool cli_shell_write_line(const char *text) {
    if ((text != NULL) && !cli_shell_write(text)) {
        return false;
    }

    return cli_shell_write_bytes((const uint8_t *)"\r\n", 2u);
}

static const cli_shell_command_t *cli_shell_find_command(const char *name) {
    for (uint32_t index = 0u; index < cli_shell_state.command_count; ++index) {
        if (strcmp(cli_shell_state.commands[index].name, name) == 0) {
            return &cli_shell_state.commands[index];
        }
    }

    return NULL;
}

static void cli_shell_dispatch_line(void) {
    const cli_shell_command_t *command;
    const char *argv[CLI_SHELL_ARG_COUNT];
    int argc = 0;
    bool in_token = false;

    cli_shell_state.line[cli_shell_state.line_length] = '\0';

    for (uint32_t index = 0u; index < cli_shell_state.line_length; ++index) {
        char byte = cli_shell_state.line[index];

        if ((byte == ' ') || (byte == '\t')) {
            cli_shell_state.line[index] = '\0';
            in_token = false;
            continue;
        }

        if (!in_token) {
            if (argc >= (int)CLI_SHELL_ARG_COUNT) {
                cli_shell_write_line("Too many arguments");
                cli_shell_state.line_length = 0u;
                return;
            }

            argv[argc] = &cli_shell_state.line[index];
            argc += 1;
            in_token = true;
        }
    }

    if (argc == 0) {
        cli_shell_state.line_length = 0u;
        return;
    }

    command = cli_shell_find_command(argv[0]);
    if (command == NULL) {
        if ((cli_shell_state.unknown_handler == NULL) || !cli_shell_state.unknown_handler(argv[0])) {
            cli_shell_write_line(cli_shell_state.unknown_message);
        }
        cli_shell_state.line_length = 0u;
        return;
    }

    command->handler(argc, argv);
    cli_shell_state.line_length = 0u;
}

static void cli_shell_process_byte(uint8_t byte) {
    if ((byte == '\r') || (byte == '\n')) {
        cli_shell_dispatch_line();
        return;
    }

    if (cli_shell_state.line_length >= (CLI_SHELL_LINE_SIZE - 1u)) {
        cli_shell_state.line_length = 0u;
        cli_shell_write_line("Line too long");
        return;
    }

    cli_shell_state.line[cli_shell_state.line_length] = (char)byte;
    cli_shell_state.line_length += 1u;
}

void cli_shell_init(const cli_shell_config_t *config) {
    memset(&cli_shell_state, 0, sizeof(cli_shell_state));

    if ((config == NULL) || (config->transport == NULL) || (config->transport->read == NULL) || (config->transport->write == NULL)) {
        return;
    }

    cli_shell_state.transport = *config->transport;
    cli_shell_state.commands = config->commands;
    cli_shell_state.command_count = config->command_count;
    cli_shell_state.unknown_message = (config->unknown_message != NULL) ? config->unknown_message : "Unknown command";
    cli_shell_state.unknown_handler = config->unknown_handler;
    cli_shell_state.initialized = true;
}

void cli_shell_poll(void) {
    uint8_t rx_data[CLI_SHELL_RX_CHUNK_SIZE];

    if (!cli_shell_state.initialized) {
        return;
    }

    while (true) {
        uint32_t rx_length = cli_shell_state.transport.read(cli_shell_state.transport.context, rx_data, sizeof(rx_data));

        if (rx_length == 0u) {
            return;
        }

        for (uint32_t index = 0u; index < rx_length; ++index) {
            cli_shell_process_byte(rx_data[index]);
        }
    }
}