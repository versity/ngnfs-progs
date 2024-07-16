/* SPDX-License-Identifier: GPL-2.0 */

#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "shared/lk/list.h"
#include "shared/log.h"
#include "shared/thread.h"
#include "shared/trace.h"

#include "cli.h"

static LIST_HEAD(commands);

#define for_each_command(cmd) \
	list_for_each_entry(cmd, &commands, head)

void cli_register(struct cli_command *cmd)
{
	list_add_tail(&cmd->head, &commands);
}

static void help(int argc, char **argv)
{
	struct cli_command *cmd;

	printf("ngnfs-cli <command> [command options..]\n\n");

	printf("available commands:\n");
	for_each_command(cmd) {
		printf("  %s\n    %s\n",
		       cmd->name, cmd->desc);
	}
	printf("\n");
}

static struct cli_command *find_command(char *name)
{
	struct cli_command *cmd;

	for_each_command(cmd) {
		if (!strcmp(name, cmd->name))
			return cmd;
	}

	return NULL;
}

int main(int argc, char **argv)
{
	struct cli_command *cmd;
	int ret;

	if (argc < 2) {
		log("missing command name argument");
		help(argc, argv);
		ret = -EINVAL;
		goto out;
	}

	cmd = find_command(argv[1]);
	if (!cmd) {
		help(argc, argv);
		log("unknown cli command '%s'", argv[1]);
		ret = -EINVAL;
		goto out;
	}

	ret = cmd->func(argc - 1, argv + 1);
out:
	return !!ret;
}
