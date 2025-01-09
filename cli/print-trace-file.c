/* SPDX-License-Identifier: GPL-2.0 */

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <inttypes.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include "shared/lk/math.h"
#include "shared/lk/time64.h"

#include "shared/compare.h"
#include "shared/format-trace.h"
#include "shared/log.h"
#include "shared/trace.h"

#include "cli/cli.h"

static struct timespec get_event_real_ts(struct ngnfs_trace_file *tfi,
					 struct ngnfs_trace_event *tev)
{
	struct timespec ts;
	u64 nsec;

	nsec = ((tev->tick - tfi->synchro_tick) * tfi->nsec_per_tick_num /
		tfi->nsec_per_tick_denom) + tfi->synchro_nsec;

	ts.tv_sec = nsec / NSEC_PER_SEC;
	ts.tv_nsec = nsec % NSEC_PER_SEC;

	return ts;
}

static u64 get_thread_nr(off_t off)
{
	return (off - TRACE_CHUNK_SIZE) / TRACE_THREAD_SIZE;
}

/*
 * events are first sorted by their tick and then by the header pointer
 * which also reflects the thread nr.
 */
static int compar_events(const void *A, const void *B)
{
	const struct ngnfs_trace_event * const *a = A;
	const struct ngnfs_trace_event * const *b = B;

	return ngnfs_compare((*a)->tick, (*b)->tick) ?: ngnfs_compare(*a, *b);
}

static int print_trace_file_func(int argc, char **argv)
{
	struct ngnfs_trace_event **events = NULL;
	struct ngnfs_trace_thread *tthr;
	struct ngnfs_trace_event *tev;
	struct ngnfs_trace_file *tfi;
	void *addr = MAP_FAILED;
	struct timespec ts;
	struct stat st;
	u64 thread_nr;
	u64 *tids = NULL;
	size_t nr_events;
	size_t off;
	size_t i;
	int fd = -1;
	int ret;

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

	ret = fstat(fd, &st);
	if (ret < 0) {
		ret = -errno;
		printf("fstat error: '%s': "ENOF"\n", argv[1], ENOA(-ret));
		goto out;
	}

	if (st.st_size < TRACE_CHUNK_SIZE) {
		printf("trace file does not contain an initial file header\n");
		ret = 0;
		goto out;
	}

	/*
	 * This ends up allocating a fraction (maybe 1/3) of the trace
	 * file size to store the highest possible number of events with
	 * the smallest id struct size.
	 */
	events = calloc(st.st_size / (sizeof(struct ngnfs_trace_event) + 8),
		      sizeof(struct ngnfs_trace_event *));
	tids = calloc(DIV_ROUND_UP(st.st_size, TRACE_THREAD_SIZE), sizeof(u64));
	if (!events || !tids) {
		ret = -errno;
		printf("calloc error: '%s': "ENOF"\n", argv[1], ENOA(-ret));
		goto out;
	}

	addr = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
	if (addr == MAP_FAILED) {
		ret = -errno;
		goto out;
	}

	tfi = addr;

	off = TRACE_CHUNK_SIZE;
	nr_events = 0;
	while (off + sizeof(struct ngnfs_trace_event) <= st.st_size) {

		if ((off - TRACE_CHUNK_SIZE) % TRACE_THREAD_SIZE == 0) {
			tthr = addr + off;

			thread_nr = get_thread_nr(off);
			tids[thread_nr] = tthr->tid;
			off += TRACE_CHUNK_SIZE;
		}

		tev = addr + off;
		if (tev->size == 0) {
			if ((off & TRACE_CHUNK_MASK) == 0) {
				thread_nr = get_thread_nr(off);
				off = TRACE_CHUNK_SIZE + (thread_nr + 1) * TRACE_THREAD_SIZE;
			} else {
				off = round_up(off, TRACE_CHUNK_SIZE);
			}
			continue;
		}

		events[nr_events++] = tev;
		off += tev->size;
	}

	qsort(events, nr_events, sizeof(struct ngnfs_trace_event *), compar_events);

	printf("# trace file metadata:\n"
	       "#   size %llu\n"
	       "#   threads %llu\n"
	       "#   endian %016llx\n"
	       "#   synchro_tick %llu\n"
	       "#   synchro_nsec %llu\n"
	       "#   nsec_per_tick_num %llu\n"
	       "#   nsec_per_tick_denom %llu\n",
			(u64)st.st_size, get_thread_nr(st.st_size),
			tfi->endian, tfi->synchro_tick, tfi->synchro_nsec,
			tfi->nsec_per_tick_num, tfi->nsec_per_tick_denom);

	printf("#\n"
	       "#    TID   NR        TIMESTAMP     NAME + ARGUMENTS\n"
               "#     |     |           |           |\n");

	for (i = 0; i < nr_events; i++) {
		tev = events[i];
		off = (void *)tev - addr;

		ts = get_event_real_ts(tfi, tev);
		thread_nr = get_thread_nr(off);

		printf("%9llu %3llu %9llu.%09u ", tids[thread_nr], thread_nr, (u64)ts.tv_sec,
		       (unsigned)ts.tv_nsec);
		print_trace_event(tev->id, tev + 1);
	}

	ret = 0;
out:
	free(events);
	free(tids);
	if (addr != MAP_FAILED)
		munmap(addr, st.st_size);
	if (fd >= 0)
		close(fd);

	return ret;
}

static struct cli_command print_trace_file_cmd = {
	.func = print_trace_file_func,
	.name = "print-trace-file",
	.desc = "print-trace-file desc",
};

CLI_REGISTER(print_trace_file_cmd);
