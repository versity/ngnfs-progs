/* SPDX-License-Identifier: GPL-2.0 */

#define _GNU_SOURCE /* gettid() */

#include <unistd.h>
#include <errno.h>
#include <sys/uio.h>
#include <fcntl.h>
#include <sys/mman.h>

#include "shared/lk/atomic.h"
#include "shared/lk/bitops.h"
#include "shared/lk/build_bug.h"
#include "shared/lk/byteorder.h"
#include "shared/lk/err.h"
#include "shared/lk/limits.h"
#include "shared/lk/list.h"
#include "shared/lk/math.h"
#include "shared/lk/minmax.h"
#include "shared/lk/mutex.h"
#include "shared/lk/timekeeping.h"
#include "shared/lk/wait.h"

#include "shared/format-trace.h"
#include "shared/thread.h"
#include "shared/trace.h"
#include "shared/urcu.h"

/*
 * This userspace tracing facility has the following goals:
 *
 * 1) Low trace event cost.  We can easily have order 10 events per IO,
 * and devices can have order 100000 IO/s.  The cpu cost of each trace
 * call is aggressively minimized, to the inconvenience of nearly
 * everything else.  Each trace event boils down to an inline struct
 * initialization.
 *
 * 2) Preserve traces as proccesses are killed without process
 * cooperation.  This first pass takes the easiest route and uses
 * mmapped files.
 *
 * 3) Implement tracing function calls that mirror the kernel's
 * TRACE_EVENT() style tracing.  We have build tooling to generate the
 * trace event implementations from event specifications.
 *
 * On trace_setup() we open the tracing file.  Each thread then maps a
 * region of the file as its buffer.  Region setup is expensive and the
 * regions are never reclaimed so this is only appropriate for a small
 * number of long lived threads.
 *
 * Variable length traces are stored in the buffer as a ring, and the
 * buffer is divided into fixed size chunks.  Each event's location is
 * rounded up to the next chunk if it would span chunks.  This ensures
 * that we can alwasys start reading from events at the start of chunks
 * as we wrap around the ring.
 */

struct trace_info {
	atomic64_t thread_nr;
	int fd;
	bool have_key;
	int page_size;
};

pthread_key_t trace_thread_private_key;

/* see comment above trace_setup() */
static struct trace_info *global_trinf = NULL;

/*
 * trace_setup() has created the file.  We fallocate and map the buffer
 * in the file for our thread nr.  This is very expensive so it won't do
 * well if we have high thread create rates (don't do that).
 */
int trace_register_thread(void)
{
	struct trace_thread_private *tpriv = NULL;
	struct trace_info *trinf = global_trinf;
	struct ngnfs_trace_thread *tthr;
	void *addr = MAP_FAILED;
	off_t page_off;
	off_t off;
	u64 nr;
	int ret;

	if (!trinf) {
		ret = 0;
		goto out;
	}

	tpriv = malloc(sizeof(struct trace_thread_private));
	if (!tpriv) {
		ret = -ENOMEM;
		goto out;
	}

	nr = atomic64_inc_return(&trinf->thread_nr);
	off = TRACE_CHUNK_SIZE + (nr * TRACE_THREAD_SIZE);
	page_off = off & (trinf->page_size - 1);

	ret = posix_fallocate(trinf->fd, off, TRACE_THREAD_SIZE);
	if (ret < 0) {
		ret = -errno;
		goto out;
	}

	/* MAP_LOCKED might be nice, but it runs into rlimit fast */
	addr = mmap(NULL, page_off + TRACE_THREAD_SIZE, PROT_READ | PROT_WRITE,
		    MAP_SHARED | MAP_POPULATE, trinf->fd, off - page_off);
	if (addr == MAP_FAILED) {
		ret = -errno;
		goto out;
	}

	tthr = addr + page_off;
	tthr->tid = gettid();

	tpriv->addr = addr;
	tpriv->buf = addr + page_off + TRACE_CHUNK_SIZE;
	tpriv->offset = 0;
	tpriv->mask = TRACE_BUF_SIZE - 1;

	ret = pthread_setspecific(trace_thread_private_key, tpriv);
	if (ret > 0) {
		ret = -ret;
		goto out;
	}

	ret = 0;
out:
	if (ret < 0) {
		if (addr != MAP_FAILED)
			munmap(addr, TRACE_BUF_SIZE);
		free(tpriv);
	}

	return ret;
}

void trace_unregister_thread(void)
{
	struct trace_thread_private *tpriv = trace_get_tpriv();
	struct trace_info *trinf = global_trinf;

	if (!trinf || !tpriv)
		return;

	munmap(tpriv->addr, TRACE_BUF_SIZE);
	free(tpriv);

	pthread_setspecific(trace_thread_private_key, NULL);
}

struct cpuid_result {
	u32 eax;
	u32 ebx;
	u32 ecx;
	u32 edx;
};

static void cpuid(u32 inp, struct cpuid_result *res)
{
	static u32 count = 0;
	asm("cpuid			\n\t"
	    : "=a" (res->eax), "=b" (res->ebx), "=c" (res->ecx), "=d" (res->edx)
	    : "0" (inp), "2" (count));
}

/*
 * SDM Vol 2A 3-237:
 *
 * EAX = 15H
 *	EAX Bits 31-00: denominator of the TSC/”core crystal clock” ratio.
 *	EBX Bits 31-00: numerator of the TSC/”core crystal clock” ratio.
 *	ECX Bits 31-00: nominal frequency of the core crystal clock in Hz.
 *
 * This is obviously wildly intel specific.  We'll need to flesh this
 * out for other platforms with a generic fallback.
 */
static void describe_ticks(struct ngnfs_trace_file *tfi)
{
	struct cpuid_result res;
	ktime_t begin_ns;
	ktime_t end_ns;
	u64 begin_tick;
	u64 end_tick;
	u64 shortest;
	int i;

	shortest = U64_MAX;
	for (i = 0; i < 10; i++) {
		begin_tick = caa_get_cycles();
		begin_ns = ktime_get_real_ns();
		end_ns = ktime_get_real_ns();
		end_tick = caa_get_cycles();

		if (end_tick > begin_tick && (end_tick - begin_tick) < shortest) {
			shortest = end_tick - begin_tick;
			tfi->synchro_tick = (begin_tick + end_tick) >> 1;
			tfi->synchro_nsec = (begin_ns + end_ns) >> 1;
		}
	}

	cpuid(0, &res);
	if (res.eax >= 0x15) {
		cpuid(0x15, &res);
		if (res.eax && res.ebx && res.ecx) {
			/* ticks / sec = res.ecx * res.ebx / res.eax */
			tfi->nsec_per_tick_num = (u64)res.eax * NSEC_PER_SEC;
			tfi->nsec_per_tick_denom = (u64)res.ecx * res.ebx;
		}
	}
}

/*
 * Initialize the global trace state that's required for thread
 * registration.  This is done very early, typically just after option
 * parsing to get the trace file name.
 *
 * The userspace tracing layer is a little different than other layers
 * that are shared with the kernel module.  It inherits its interface
 * from the kernel trace_ngnfs_* interface which has no
 * per-(filesystem,mount,superblock) state.  It's global.  So our
 * trace_info isn't stored in the ngnfs_fs_info, it's also global.  That
 * doesn't really matter for our userspace processes which only ever
 * have one filesystem but it explains why this setup doesn't take an
 * nfi argument and why we have an unconventional global_trinf pointer.
 */
int trace_setup(char *trace_path)
{
	struct ngnfs_trace_file *tfi = NULL;
	struct trace_info *trinf;
	int ret;

	trinf = malloc(sizeof(struct trace_info));
	tfi = calloc(1, TRACE_CHUNK_SIZE);
	if (!trinf || !tfi) {
		ret = -ENOMEM;
		goto out;
	}

	atomic64_set(&trinf->thread_nr, U64_MAX); /* 0 is first assigned */
	trinf->fd = -1;
	trinf->have_key = false;
	trinf->page_size = sysconf(_SC_PAGE_SIZE);

	global_trinf = trinf;

	trinf->fd = open(trace_path, O_CREAT | O_RDWR | O_TRUNC, 0644);
	if (trinf->fd < 0) {
		ret = -errno;
		goto out;
	}

	tfi->endian = NGNFS_TRACE_NATIVE_ENDIAN;
	tfi->synchro_tick = 0;
	tfi->synchro_nsec = 0;
	tfi->nsec_per_tick_num = 1;
	tfi->nsec_per_tick_denom = 1;

	describe_ticks(tfi);

	ret = pwrite(trinf->fd, tfi, TRACE_CHUNK_SIZE, 0);
	if (ret != TRACE_CHUNK_SIZE) {
		if (ret >= 0)
			ret = -EIO;
		else
			ret = -errno;
		goto out;
	}

	ret = pthread_key_create(&trace_thread_private_key, NULL);
	if (ret > 0) {
		ret = -ret;
		goto out;
	}

	trinf->have_key = true;
	ret = 0;
out:
	free(tfi);
	if (ret < 0)
		trace_destroy();

	return ret;
}

/*
 * Fully tear down tracing.  This is called after all other trace users
 * have stopped.
 */
void trace_destroy(void)
{
	struct trace_info *trinf = global_trinf;

	if (trinf) {
		if (trinf->fd >= 0)
			close(trinf->fd);
		if (trinf->have_key)
			pthread_key_delete(trace_thread_private_key);
		free(trinf);
		global_trinf = NULL;
	}
}
