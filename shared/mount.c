/* SPDX-License-Identifier: GPL-2.0 */

/*
 * A simple mount/unmount for userspace processes.
 */

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <ctype.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "shared/lk/kernel.h"
#include "shared/lk/list.h"
#include "shared/lk/types.h"

#include "shared/block.h"
#include "shared/btr-msg.h"
#include "shared/log.h"
#include "shared/manifest.h"
#include "shared/mount.h"
#include "shared/msg.h"
#include "shared/mtr-socket.h"
#include "shared/nerr.h"
#include "shared/options.h"
#include "shared/parse.h"
#include "shared/trace.h"

struct mount_options {
	struct list_head addr_list;
	u8 nr_addrs;
	char *trace_path;
};

static struct option_more mount_moreopts[] = {
	{ .longopt = { "devd_addr", required_argument, NULL, 'd' },
	  .arg = "addr:port",
	  .desc = "IPv4 address of devd server", },

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
	case 'd':
		ret = ngnfs_manifest_append_addr(&opts->nr_addrs, &opts->addr_list, str);
		break;
	case 't':
		ret = strdup_nerr(&opts->trace_path, str);
		break;
	}

	return ret;
}

int ngnfs_mount(struct ngnfs_fs_info *nfi, int argc, char **argv)
{
	struct mount_options opts = { .addr_list = LIST_HEAD_INIT(opts.addr_list), };
	int ret;

	ret = getopt_long_more(argc, argv, mount_moreopts, ARRAY_SIZE(mount_moreopts),
			       parse_mount_opt, &opts);
	if (ret < 0)
		goto out;

	if (opts.nr_addrs == 0) {
		log("no -d devd addresses specified");
		ret = -EINVAL;
		goto out;
	}

	ret = trace_setup(opts.trace_path) ?:
	      ngnfs_manifest_setup(nfi, &opts.addr_list, opts.nr_addrs) ?:
	      ngnfs_msg_setup(nfi, &ngnfs_mtr_socket_ops, NULL, NULL) ?:
	      ngnfs_block_setup(nfi, &ngnfs_btr_msg_ops, NULL);
out:
	if (ret < 0)
		ngnfs_unmount(nfi);

	ngnfs_manifest_free_addrs(&opts.addr_list);

	return ret;
}

void ngnfs_unmount(struct ngnfs_fs_info *nfi)
{
	ngnfs_block_destroy(nfi);
	ngnfs_msg_destroy(nfi);
	ngnfs_manifest_destroy(nfi);
}
