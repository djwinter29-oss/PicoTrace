/**
 * @file device_cli.h
 * @brief Device-specific CLI entrypoints layered on top of the generic shell.
 */

#ifndef DEVICE_CLI_H
#define DEVICE_CLI_H

#include "cli/cli_shell.h"

/**
 * @brief Initialize the device-specific CLI command set.
 * @param transport Caller-owned transport binding used by the generic shell.
 */
void device_cli_init(const cli_shell_transport_t *transport);

/** @brief Poll the device-specific CLI transport and dispatch any complete commands. */
void device_cli_poll(void);

#endif