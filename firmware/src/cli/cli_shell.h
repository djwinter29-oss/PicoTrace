/**
 * @file cli_shell.h
 * @brief Minimal line-oriented command shell shared by the PicoTrace CLI surfaces.
 */

#ifndef CLI_SHELL_H
#define CLI_SHELL_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Transport read callback used by the CLI shell.
 * @param context Caller-owned transport context.
 * @param data Destination buffer for received bytes.
 * @param capacity Maximum bytes to copy into @p data.
 * @return Number of bytes copied into @p data.
 */
typedef uint32_t (*cli_shell_transport_read_t)(void *context, uint8_t *data, uint32_t capacity);

/**
 * @brief Transport write callback used by the CLI shell.
 * @param context Caller-owned transport context.
 * @param data Source buffer containing bytes to write.
 * @param length Number of bytes to write.
 * @return `true` when the bytes were accepted, otherwise `false`.
 */
typedef bool (*cli_shell_transport_write_t)(void *context, const uint8_t *data, uint32_t length);

/** @brief Transport binding used by the CLI shell for byte-oriented I/O. */
typedef struct {
    cli_shell_transport_read_t read; /**< Callback used to fetch received bytes. */
    cli_shell_transport_write_t write; /**< Callback used to write response bytes. */
    void *context; /**< Caller-owned context passed back to @ref read and @ref write. */
} cli_shell_transport_t;

/**
 * @brief Command handler invoked for one parsed CLI command line.
 * @param argc Number of parsed argument tokens.
 * @param argv Null-terminated token strings backed by the shell line buffer.
 * @return `true` when the handler completed normally, otherwise `false`.
 */
typedef bool (*cli_shell_command_handler_t)(int argc, const char *const *argv);

/**
 * @brief Unknown-command handler invoked when no registered command matches the first token.
 * @param command_name First token from the unrecognized command line.
 * @return `true` when the handler already produced its own response, otherwise `false`.
 */
typedef bool (*cli_shell_unknown_handler_t)(const char *command_name);

/** @brief One registered CLI command entry. */
typedef struct {
    const char *name; /**< Command token matched against the first word in a line. */
    const char *help; /**< One-line help text shown by the device CLI help command. */
    cli_shell_command_handler_t handler; /**< Callback invoked when @ref name matches. */
} cli_shell_command_t;

/** @brief Generic shell configuration supplied during initialization. */
typedef struct {
    const cli_shell_transport_t *transport; /**< Transport binding used for shell I/O. */
    const cli_shell_command_t *commands; /**< Static command table visible to the shell. */
    uint32_t command_count; /**< Number of entries in @ref commands. */
    const char *unknown_message; /**< Fallback message used when no unknown-handler responds. */
    cli_shell_unknown_handler_t unknown_handler; /**< Optional callback for unknown command names. */
} cli_shell_config_t;

/**
 * @brief Initialize the generic CLI shell.
 * @param config Caller-owned shell configuration.
 */
void cli_shell_init(const cli_shell_config_t *config);

/** @brief Poll the shell transport, assemble input lines, and dispatch complete commands. */
void cli_shell_poll(void);

/**
 * @brief Write raw text through the shell transport.
 * @param text Null-terminated text to write.
 * @return `true` when the text was accepted, otherwise `false`.
 */
bool cli_shell_write(const char *text);

/**
 * @brief Write one text line followed by CRLF through the shell transport.
 * @param text Null-terminated line text, or `NULL` to emit only CRLF.
 * @return `true` when the line was accepted, otherwise `false`.
 */
bool cli_shell_write_line(const char *text);

#endif