/* SPDX-License-Identifier: GPL-2.0 */

/*
 * A simple mount/unmount for userspace processes.
 */

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "shared/lk/kernel.h"

#include "shared/block.h"
#include "shared/btr-msg.h"
#include "shared/lk/wait.h"
#include "shared/log.h"
#include "shared/map.h"
#include "shared/mount.h"
#include "shared/msg.h"
#include "shared/mtr-socket.h"
#include "shared/nerr.h"
#include "shared/options.h"
#include "shared/parse.h"
#include "shared/shutdown.h"
#include "shared/trace.h"

struct mount_options {
	struct sockaddr_in mapd_server_addr;
	char *trace_path;
};

static struct option_more mount_moreopts[] = {
	{ .longopt = { "addr", required_argument, NULL, 'a' },
	  .arg = "addr:port",
	  .desc = "IPv4 address and port of mapd server to query",
	  .required = 1, },

	{ .longopt = { "trace_file", required_argument, NULL, 't' },
	  .arg = "file_path",
	  .desc = "append debugging traces to this file",
	  .required = 1, },
};

static int parse_mount_opt(int c, char *str, void *arg)
{
	struct mount_options *opts = arg;
	int ret = -EINVAL;

	switch(c) {
	case 'a':
		ret = parse_ipv4_addr_port(&opts->mapd_server_addr, str);
	case 't':
		ret = strdup_nerr(&opts->trace_path, str);
		break;
	}

	return ret;
}

int ngnfs_mount(struct ngnfs_fs_info *nfi, int argc, char **argv)
{
	struct mount_options opts = { };
	int ret;

	ret = getopt_long_more(argc, argv, mount_moreopts, ARRAY_SIZE(mount_moreopts),
			       parse_mount_opt, &opts);
	if (ret < 0)
		goto out;

	ret = trace_setup(opts.trace_path) ?:
	      ngnfs_map_setup(nfi) ?:
	      ngnfs_msg_setup(nfi, &ngnfs_mtr_socket_ops, NULL, NULL) ?:
	      ngnfs_block_setup(nfi, &ngnfs_btr_msg_ops, NULL) ?:
	      ngnfs_map_client_setup(nfi, &opts.mapd_server_addr)? :
	      ngnfs_map_request_maps(nfi);

out:
	if (ret < 0)
		ngnfs_unmount(nfi);

	return ret;
}

void ngnfs_unmount(struct ngnfs_fs_info *nfi)
{
	ngnfs_shutdown(nfi, 0);
	ngnfs_map_client_destroy(nfi);
	ngnfs_block_destroy(nfi);
	ngnfs_msg_destroy(nfi);
	ngnfs_map_destroy(nfi);
}
