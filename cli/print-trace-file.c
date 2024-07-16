/* SPDX-License-Identifier: GPL-2.0 */

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <inttypes.h>
#include <fcntl.h>

#include "shared/lk/byteorder.h"
#include "shared/lk/err.h"

#include "shared/format-trace.h"
#include "shared/log.h"
#include "shared/trace.h"

#include "cli/cli.h"

#define BUF_SIZE (8 * 1024 * 1024)

static int print_trace_file_func(int argc, char **argv)
{
	struct ngnfs_trace_event_header *hdr;
	void *buf = NULL;
	int fd = -1;
	ssize_t sret;
	size_t size;
	size_t off;
	int ret;

	buf = malloc(BUF_SIZE);
	if (!buf) {
		ret = -errno;
		goto out;
	}

	/*
	 * XXX how are we doing option parsing in commands?
	 */
	if (argc != 2) {
		printf("incorrect argc %d\n", argc);
		ret = -EINVAL;
		goto out;
	}

	fd = open(argv[1], O_RDONLY);
	if (fd < 0) {
		ret = -errno;
		printf("error opening '%s': "ENOF"\n", argv[1], ENOA(-ret));
		goto out;
	}

	size = 0;
	for (;;) {
		sret = read(fd, buf + size, BUF_SIZE - size);
		if (sret <= 0) {
			if (sret == 0)
				break;
			ret = -errno;
			goto out;
		}

		size += sret;

		off = 0;

		while (off + sizeof(*hdr) <= size) {

			hdr = buf + off;
			if (off + le16_to_cpu(hdr->size) > size)
				break;

			print_trace_event(le16_to_cpu(hdr->id), buf + off + sizeof(*hdr));
			off += le16_to_cpu(hdr->size);
		}

		if (off < size) {
			memmove(buf, buf + off, size - off);
			size -= off;
		}
	}

	ret = 0;
out:
	if (fd >= 0)
		close(fd);
	free(buf);

	return ret;
}

static struct cli_command print_trace_file_cmd = {
	.func = print_trace_file_func,
	.name = "print-trace-file",
	.desc = "print-trace-file desc",
};

CLI_REGISTER(print_trace_file_cmd);
