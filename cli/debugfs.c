/* SPDX-License-Identifier: GPL-2.0 */

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <inttypes.h>

#include "shared/lk/byteorder.h"
#include "shared/lk/err.h"
#include "shared/lk/kernel.h"
#include "shared/lk/timekeeping.h"
#include "shared/lk/types.h"

#include "shared/format-block.h"
#include "shared/log.h"
#include "shared/mount.h"
#include "shared/pfs.h"
#include "shared/thread.h"
#include "shared/txn.h"

#include "cli/cli.h"

struct debugfs_context {
	struct ngnfs_fs_info *nfi;
	u64 cwd_ino;
};

#define LINE_SIZE (PATH_MAX * 5)
#define MAX_ARGC ((LINE_SIZE + 1) / 2)

static void cmd_mkfs(struct debugfs_context *ctx, int argc, char **argv)
{
	struct ngnfs_transaction txn = INIT_NGNFS_TXN(txn);
	int ret;

	ret = ngnfs_pfs_mkfs(ctx->nfi, &txn, NGNFS_ROOT_INO, ktime_get_real_ns());
	ngnfs_txn_destroy(ctx->nfi, &txn);
	if (ret < 0) {
		printf("mkfs error: "ENOF"\n", ENOA(-ret));
		return;
	}

	ret = ngnfs_block_sync(ctx->nfi);
	if (ret < 0)
		printf("final sync error: "ENOF"\n", ENOA(-ret));
}

static void cmd_stat(struct debugfs_context *ctx, int argc, char **argv)
{
	struct ngnfs_transaction txn = INIT_NGNFS_TXN(txn);
	struct ngnfs_inode ninode;
	int ret;

	ret = ngnfs_pfs_read_inode(ctx->nfi, &txn, NGNFS_ROOT_INO, &ninode, sizeof(ninode));
	ngnfs_txn_destroy(ctx->nfi, &txn);

	if (ret < 0) {
		log("stat error: %d", ret);
	} else if (ret < sizeof(ninode)) {
		log("returned inode buffer size %d too small, wanted at least %zu",
		    ret, sizeof(ninode));
	} else {
		printf("ino: %llu\n"
		       "gen: %llu\n"
		       "nlink: %u\n"
		       "mode: %o\n"
		       "atime: %llu\n"
		       "ctime: %llu\n"
		       "mtime: %llu\n"
		       "crtime: %llu\n",
		       le64_to_cpu(ninode.ino),
		       le64_to_cpu(ninode.gen),
		       le32_to_cpu(ninode.nlink),
		       le32_to_cpu(ninode.mode),
		       le64_to_cpu(ninode.atime_nsec),
		       le64_to_cpu(ninode.ctime_nsec),
		       le64_to_cpu(ninode.mtime_nsec),
		       le64_to_cpu(ninode.crtime_nsec));
	}
}

static struct command {
	char *name;
	void (*func)(struct debugfs_context *ctx, int argc, char **argv);
} commands[] = {
	{ "mkfs", cmd_mkfs, },
	{ "stat", cmd_stat, },
};

static int compar_cmd_names(const void *A, const void *B)
{
	const struct command *a = A;
	const struct command *b = B;

	return strcmp(a->name, b->name);
}

static int compar_key_cmd_name(const void *key, const void *ele)
{
	const char *name = key;
	const struct command *cmd = ele;

	return strcmp(name, cmd->name);
}

static void parse_command(struct debugfs_context *ctx, char *buf, char **argv)
{
	struct command *cmd;
	char *delim = "\t \n\r";
	char *saveptr;
	char *str;
	int argc;

	for (argc = 0, str = buf, saveptr = NULL; argc < MAX_ARGC; argc++, str = NULL) {
		argv[argc] = strtok_r(str, delim, &saveptr);
		if (argv[argc] == NULL)
			break;
	}

	if (argc == 0) {
		printf("no command");
		return;
	}

	cmd = bsearch(argv[0], commands, ARRAY_SIZE(commands), sizeof(commands[0]),
		      compar_key_cmd_name);
	if (!cmd) {
		printf("unknown command: '%s'\n", argv[0]);
		return;
	}

	cmd->func(ctx, argc, argv);
}

struct debugfs_thread_args {
	int argc;
	char **argv;
	int ret;
};

static void debugfs_thread(struct thread *thr, void *arg)
{
	struct debugfs_thread_args *dargs = arg;
	struct ngnfs_fs_info nfi = INIT_NGNFS_FS_INFO;
	struct debugfs_context _ctx = {
		.nfi = &nfi,
		.cwd_ino = NGNFS_ROOT_INO,
	}, *ctx = &_ctx;
	char **line_argv = NULL;
	char *line = NULL;
	int ret;

	line = malloc(LINE_SIZE);
	line_argv = calloc(MAX_ARGC, sizeof(line_argv[0]));
	if (!line || !line_argv) {
		ret = -ENOMEM;
		goto out;
	}

	ret = ngnfs_mount(&nfi, dargs->argc, dargs->argv);
	if (ret < 0)
		goto out;

	/* make sure command names are sorted for bsearch */
	qsort(commands, ARRAY_SIZE(commands), sizeof(commands[0]), compar_cmd_names);

	for (;;) {
		fprintf(stdout, "<%llu> $ ", ctx->cwd_ino);
		fflush(stdout);
		if (!fgets(line, LINE_SIZE, stdin))
			break;

		parse_command(ctx, line, line_argv);
	}

	ngnfs_unmount(&nfi);
	ret = 0;
out:
	free(line);
	free(line_argv);
	dargs->ret = ret;
}

/*
 * We have the debugfs command run in a thread so that it can call ngnfs
 * client operations (pfs, block, txn) directly.  That dictates its
 * signal handling behaviour and makes it uninterruptible.  We park this
 * initial cli command function as a monitoring thread that can stop the
 * debugfs thread when it catches signals.
 */
static int debugfs_func(int argc, char **argv)
{
	struct debugfs_thread_args dargs = {
		.argc = argc,
		.argv = argv,
	};
	struct thread thr;
	int ret;

	thread_init(&thr);

	ret = thread_prepare_main();
	if (ret < 0)
		goto out;

	ret = thread_start(&thr, debugfs_thread, &dargs) ?:
	      thread_sigwait();

	thread_stop_wait(&thr);

out:
	thread_finish_main();

	return ret ?: dargs.ret;
}

static struct cli_command debugfs_cmd = {
	.func = debugfs_func,
	.name = "debugfs",
	.desc = "debugfs desc",
};

CLI_REGISTER(debugfs_cmd);
