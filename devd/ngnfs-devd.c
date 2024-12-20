/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Each devd process handles incoming network requests using a single
 * device.
 */

#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "shared/lk/err.h"
#include "shared/lk/kernel.h"

#include "shared/block.h"
#include "shared/daemon.h"
#include "shared/log.h"
#include "shared/msg.h"
#include "shared/mtr-socket.h"
#include "shared/nerr.h"
#include "shared/options.h"
#include "shared/parse.h"
#include "shared/thread.h"
#include "shared/trace.h"

#include "devd/recv.h"
#include "devd/btr-aio.h"

struct devd_options {
	char *dev_path;
	struct sockaddr_in listen_addr;
	bool foreground;
	char *trace_path;
};

static struct option_more devd_moreopts[] = {
	{ .longopt = { "device_path", required_argument, NULL, 'd' },
	  .arg = "path",
	  .desc = "path to block device",
	  .required = 1, },

	{ .longopt = { "foreground", no_argument, NULL, 'f' },
	  .desc = "do not daemonize and run in the foreground",
	  .required = 0, },

	{ .longopt = { "listen_addr", required_argument, NULL, 'l' },
	  .arg = "addr:port",
	  .desc = "listening IPv4 address and port",
	  .required = 1, },

	{ .longopt = { "trace_file", required_argument, NULL, 't' },
	  .arg = "file_path",
	  .desc = "append debugging traces to this file",
	  .required = 1, },
};

static int parse_devd_opt(int c, char *str, void *arg)
{
	struct devd_options *opts = arg;
	int ret = -EINVAL;

	switch(c) {
	case 'd':
		ret = strdup_nerr(&opts->dev_path, str);
		break;
	case 'f':
		opts->foreground = true;
		ret = 0;
		break;
	case 'l':
		ret = parse_ipv4_addr_port(&opts->listen_addr, str);
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
	struct devd_options opts = { };
	int pipefd[2];
	int ret;

	ret = getopt_long_more(argc, argv, devd_moreopts, ARRAY_SIZE(devd_moreopts),
			       parse_devd_opt, &opts);
	if (ret < 0)
		goto out;

	if (!opts.foreground)
		ret = daemonize(pipefd);

	if (ret < 0)
		goto out;

	ret = thread_prepare_main() ?:
	      trace_setup(opts.trace_path) ?:
	      ngnfs_msg_setup(&nfi, &ngnfs_mtr_socket_ops, NULL, &opts.listen_addr) ?:
	      ngnfs_block_setup(&nfi, &ngnfs_btr_aio_ops, opts.dev_path) ?:
	      devd_recv_setup(&nfi);

	if (!opts.foreground)
		daemon_report(pipefd, ret);

	if (ret == 0)
		thread_sigwait();

	devd_recv_destroy(&nfi);
	ngnfs_block_destroy(&nfi);
	ngnfs_msg_destroy(&nfi);

	thread_finish_main();
out:
	return !!ret;
}
