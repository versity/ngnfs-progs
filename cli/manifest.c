/* SPDX-License-Identifier: GPL-2.0 */

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <inttypes.h>

#include <sys/socket.h>
#include <netinet/in.h>

#include "shared/lk/kernel.h"
#include "shared/lk/types.h"

#include "shared/fs_info.h"
#include "shared/log.h"
#include "shared/manifest.h"
#include "shared/msg.h"
#include "shared/mtr-socket.h"
#include "shared/nerr.h"
#include "shared/options.h"
#include "shared/parse.h"
#include "shared/thread.h"
#include "shared/trace.h"

#include "cli/cli.h"

struct manifest_options {
	struct sockaddr_in manifest_server_addr;
	char *trace_path;
};

static struct option_more manifest_moreopts[] = {
	{ .longopt = { "addr", required_argument, NULL, 'a' },
	  .arg = "addr:port",
	  .desc = "IPv4 address and port of manifest server to query",
	  .required = 1, },

	{ .longopt = { "trace_file", required_argument, NULL, 't' },
	  .arg = "file_path",
	  .desc = "append debugging traces to this file",
	  .required = 1, },
};

static int parse_manifest_opt(int c, char *str, void *arg)
{
	struct manifest_options *opts = arg;
	int ret = -EINVAL;

	switch(c) {
	case 'a':
		ret = parse_ipv4_addr_port(&opts->manifest_server_addr, str);
		break;
	case 't':
		ret = strdup_nerr(&opts->trace_path, str);
		break;
	}

	return ret;
}

static int manifest_func(int argc, char **argv)
{
	struct ngnfs_fs_info nfi = INIT_NGNFS_FS_INFO;
	int ret;

	struct manifest_options opts = { };

	ret = getopt_long_more(argc, argv, manifest_moreopts,
						   ARRAY_SIZE(manifest_moreopts), parse_manifest_opt,
						   &opts);
	if (ret < 0)
		goto out;

	ret = thread_prepare_main();
	if (ret < 0)
		goto out;

	ret = trace_setup(opts.trace_path) ?:
	      ngnfs_msg_setup(&nfi, &ngnfs_mtr_socket_ops, NULL, NULL) ?:
	      ngnfs_manifest_client_setup(&nfi, &opts.manifest_server_addr, NULL, 0);

	if (ret < 0)
		log("error requesting map: "ENOF, ENOA(-ret));
	else
		log("map received");

	ngnfs_manifest_client_destroy(&nfi);
	ngnfs_msg_destroy(&nfi);
	thread_finish_main();
out:
	return !!ret;
}

static struct cli_command manifest_cmd = {
	.func = manifest_func,
	.name = "manifest",
	.desc = "manifest request an updated manifest",
};

CLI_REGISTER(manifest_cmd);
