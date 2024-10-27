/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Each mapd server keeps lists of properties of the ngnfs cluster that
 * all the nodes need to agree on (such as which nodes are currently up
 * and running). A quorum of mapd servers needs to agree on any changes
 * (including changes in which mapd servers are up and participating in
 * quorum).
 */

#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "shared/lk/kernel.h"
#include "shared/lk/limits.h"

#include "shared/log.h"
#include "shared/map.h"
#include "shared/msg.h"
#include "shared/mtr-socket.h"
#include "shared/nerr.h"
#include "shared/options.h"
#include "shared/parse.h"
#include "shared/thread.h"
#include "shared/trace.h"

#include "mapd/recv.h"

struct mapd_options {
	char *storage_dir;
	struct sockaddr_in listen_addr;
	struct list_head addr_list;
	u8 nr_addrs;
	char *trace_path;
};

/* Parse the IPv4 addr:port in str and add it to addr_list. */
static int map_append_addr(u8 *nr_addrs, struct list_head *addr_list, char *str)
{
	struct ngnfs_map_addr_head *ahead;
	int ret;

	if (*nr_addrs == U8_MAX) {
		log("too many -d addresses specified, exceeded limit of %u", U8_MAX);
		return -EINVAL;
	}

	ahead = malloc(sizeof(struct ngnfs_map_addr_head));
	if (!ahead)
		return -ENOMEM;

	ret = parse_ipv4_addr_port(&ahead->addr, str);
	if (ret < 0) {
		log("error parsing -d address");
		goto out;
	}

	list_add_tail(&ahead->head, addr_list);
	(*nr_addrs)++;
	return ret;
out:
	free(ahead);
	return ret;
}

void ngnfs_map_free_addrs(struct list_head *addr_list)
{
	struct ngnfs_map_addr_head *ahead;
	struct ngnfs_map_addr_head *tmp;

	list_for_each_entry_safe(ahead, tmp, addr_list, head) {
		list_del_init(&ahead->head);
		free(ahead);
	}
}

static struct option_more mapd_moreopts[] = {
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

static int parse_mapd_opt(int c, char *str, void *arg)
{
	struct mapd_options *opts = arg;
	int ret = -EINVAL;

	switch(c) {
	case 's':
		ret = strdup_nerr(&opts->storage_dir, str);
		break;
	case 'l':
		ret = parse_ipv4_addr_port(&opts->listen_addr, str);
		break;
	case 'd':
		ret = map_append_addr(&opts->nr_addrs, &opts->addr_list, str);
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
	struct mapd_options opts = { .addr_list = LIST_HEAD_INIT(opts.addr_list), };
	int ret;

	ret = getopt_long_more(argc, argv, mapd_moreopts, ARRAY_SIZE(mapd_moreopts), parse_mapd_opt,
			       &opts);
	if (ret < 0)
		goto out;

	ret = thread_prepare_main();
	if (ret < 0)
		goto out;

	ret = trace_setup(opts.trace_path) ?:
	      ngnfs_map_setup(&nfi) ?:
	      ngnfs_msg_setup(&nfi, &ngnfs_mtr_socket_ops, NULL, &opts.listen_addr) ?:
	      mapd_setup(&nfi, &opts.addr_list, opts.nr_addrs) ?:
	      thread_sigwait();

	mapd_destroy(&nfi);
	ngnfs_msg_destroy(&nfi);
	ngnfs_map_destroy(&nfi);

	thread_finish_main();
out:
	ngnfs_map_free_addrs(&opts.addr_list);
	return !!ret;
}
