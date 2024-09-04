/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Each manifest server keeps lists of properties of the ngnfs cluster that all
 * the nodes need to agree on (such as which nodes are currently up and
 * running). A quorum of manifest servers needs to agree on any changes
 * (including changes in which manifest servers are up and participating in
 * quorum).
 */

#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <ctype.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "shared/lk/err.h"
#include "shared/lk/kernel.h"
#include "shared/log.h"
#include "shared/manifest.h"
#include "shared/msg.h"
#include "shared/mtr-socket.h"
#include "shared/nerr.h"
#include "shared/options.h"
#include "shared/parse.h"
#include "shared/thread.h"
#include "shared/trace.h"

struct manifest_options {
	char *storage_dir;
	struct sockaddr_in listen_addr;
	struct list_head addr_list;
	u8 nr_addrs;
	char *trace_path;
};

static struct option_more manifest_moreopts[] = {
	{ .longopt = { "storage_dir", required_argument, NULL, 's' },
	  .arg = "dir_path",
	  .desc = "path to directory used to store persistent data",
	  .required = 1, },

	{ .longopt = { "listen_addr", required_argument, NULL, 'l' },
	  .arg = "addr:port",
	  .desc = "listening IPv4 address and port",
	  .required = 1, },

	{ .longopt = { "devd_addr", required_argument, NULL, 'd' },
	  .arg = "addr:port",
	  .desc = "IPv4 address of devd server",
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
	case 's':
		ret = strdup_nerr(&opts->storage_dir, str);
		break;
	case 'l':
		ret = parse_ipv4_addr_port(&opts->listen_addr, str);
		break;
	case 'd':
		ret = ngnfs_manifest_append_addr(&opts->nr_addrs, &opts->addr_list, str);
		break;
	case 't':
		ret = strdup_nerr(&opts->trace_path, str);
		break;
	}

	return ret;
}

int main(int argc, char **argv)
{
	struct ngnfs_fs_info nfi = INIT_NGNFS_FS_INFO;
	struct manifest_options opts = { .addr_list = LIST_HEAD_INIT(opts.addr_list), };
	int ret;

	ret = getopt_long_more(argc, argv, manifest_moreopts,
						   ARRAY_SIZE(manifest_moreopts),
						   parse_manifest_opt, &opts);
	if (ret < 0)
		goto out;

	ret = thread_prepare_main();
	if (ret < 0)
		goto out;

	ret = trace_setup(opts.trace_path) ?:
	      ngnfs_msg_setup(&nfi, &ngnfs_mtr_socket_ops, NULL, &opts.listen_addr) ?:
	      ngnfs_manifest_server_setup(&nfi, &opts.addr_list, opts.nr_addrs) ?:
	      thread_sigwait();

	ngnfs_manifest_server_destroy(&nfi);
	ngnfs_msg_destroy(&nfi);

	thread_finish_main();
out:
	ngnfs_manifest_free_addrs(&opts.addr_list);
	return !!ret;
}
