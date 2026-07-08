#ifndef DEVICE_CLI_H
#define DEVICE_CLI_H

#include "cli/cli_shell.h"

void device_cli_init(const cli_shell_transport_t *transport);
void device_cli_poll(void);

#endif