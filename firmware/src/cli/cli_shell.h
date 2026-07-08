#ifndef CLI_SHELL_H
#define CLI_SHELL_H

#include <stdbool.h>
#include <stdint.h>

typedef uint32_t (*cli_shell_transport_read_t)(void *context, uint8_t *data, uint32_t capacity);
typedef bool (*cli_shell_transport_write_t)(void *context, const uint8_t *data, uint32_t length);

typedef struct {
    cli_shell_transport_read_t read;
    cli_shell_transport_write_t write;
    void *context;
} cli_shell_transport_t;

typedef bool (*cli_shell_command_handler_t)(int argc, const char *const *argv);
typedef bool (*cli_shell_unknown_handler_t)(const char *command_name);

typedef struct {
    const char *name;
    const char *help;
    cli_shell_command_handler_t handler;
} cli_shell_command_t;

typedef struct {
    const cli_shell_transport_t *transport;
    const cli_shell_command_t *commands;
    uint32_t command_count;
    const char *unknown_message;
    cli_shell_unknown_handler_t unknown_handler;
} cli_shell_config_t;

void cli_shell_init(const cli_shell_config_t *config);
void cli_shell_poll(void);
bool cli_shell_write(const char *text);
bool cli_shell_write_line(const char *text);

#endif