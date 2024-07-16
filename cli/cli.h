/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_CLI_CLI_H
#define NGNFS_CLI_CLI_H

#include "shared/lk/list.h"

struct cli_command {
	struct list_head head;
	int (*func)(int argc, char **argv);
	char *name;
	char *desc;
};

void cli_register(struct cli_command *cmd);

#define CLI_REGISTER(cmd)					\
__attribute__((constructor)) static void _register_cmd(void) {	\
	cli_register(&cmd);					\
}

#endif
