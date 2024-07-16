/* SPDX-License-Identifier: GPL-2.0 */

/*
 * devd's aio block transport uses the aio io_ syscalls to read and
 * write blocks from its private block device.  It preallocates the
 * resources to keep a fixed queue depth of block IOs in flight.  Long
 * lived threads block waiting for prepared iocbs to submit or for
 * completed io events to arrive.
 *
 * Using a static pool of iocbs creates contention on the bitmaps that
 * describe the state of the iocbs.  Managing these atomics seems better
 * than the implicit allocator contention between allocating producers
 * and freeing consumers of dynamically allocated iocbs.
 */

#define _GNU_SOURCE /* O_DIRECT */

#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/eventfd.h>
#include <linux/aio_abi.h>

#include "shared/lk/cache.h"
#include "shared/lk/bitops.h"
#include "shared/lk/bug.h"
#include "shared/lk/err.h"
#include "shared/lk/types.h"
#include "shared/lk/wait.h"

#include "shared/block.h"
#include "shared/format-block.h"
#include "shared/log.h"
#include "shared/thread.h"

#include "devd/btr-aio.h"

/* maybe excessive, but comes from iocb bmaps, -1 for sloppy initialization */
#define AIO_QUEUE_DEPTH	(BITS_PER_LONG - 1)

/*
 * Most everything here is read-{only,mostly} with the exception of iocb
 * bitmaps and the waitq so we carve them off in their own cacheline.
 * There's full connectivity of sharing relationships between the three
 * actors (btr submit caller, submit thread, getevents thread) so we put
 * the contentious fields in one cacheline and hopefully let them get
 * their work done quickly after one miss.
 */
struct btr_aio_info {
	struct ngnfs_fs_info *nfi;
	aio_context_t ctx;
	unsigned int queue_depth;
	int dev_fd;

	struct thread submit_thr;
	struct thread getevents_thr;

	struct iocb *iocbs;
	struct iocb **iocbps;
	struct io_event *events;

	unsigned long empty_bmap ____cacheline_aligned;
	unsigned long submit_bmap;
	wait_queue_head_t submit_waitq;
};

static inline int iocb_bit_nr(struct btr_aio_info *ainf, struct iocb *iocb)
{
	int nr = iocb - ainf->iocbs;

	BUG_ON(nr < 0 || nr >= ainf->queue_depth);
	return nr;
}

static inline void set_iocb_bit(struct btr_aio_info *ainf, struct iocb *iocb, unsigned long *bmap)
{
	set_bit(iocb_bit_nr(ainf, iocb), bmap);
}

static inline void clear_iocb_bit(struct btr_aio_info *ainf, struct iocb *iocb, unsigned long *bmap)
{
	clear_bit(iocb_bit_nr(ainf, iocb), bmap);
}

static inline struct iocb *get_and_clear_iocb_bit(struct btr_aio_info *ainf, unsigned long *bmap)
{
	struct iocb *iocb = NULL;
	unsigned long bits;
	int nr;

	while ((bits = READ_ONCE(*bmap))) {
		nr = __ffs(bits);
		if (!test_and_clear_bit(nr, bmap)) {
			caa_cpu_relax();
			continue;
		}
		iocb = &ainf->iocbs[nr];
	}

	return iocb;
}

/*
 * Send completion results back to the block cache.  It is updating its
 * accounting of blocks in flight with each completion and will submit
 * more blocks to saturate queue depth.
 */
static void getevents_thread(struct thread *thr, void *arg)
{
	struct btr_aio_info *ainf = arg;
	struct io_event *event;
	struct page *data_page;
	struct iocb *iocb;
	u64 bnr;
	int ret;
	int err;
	int nr;
	int i;

	while (!thread_should_return(thr)) {

		ret = syscall(__NR_io_getevents, ainf->ctx, 1, ainf->queue_depth,
			      ainf->events, NULL);
		assert(ret > 0);
		nr = ret;

		for (i = 0; i < nr; i++) {
			event = &ainf->events[i];
			data_page = (struct page *)event->data;
			iocb = (struct iocb *)event->obj;
			bnr = iocb->aio_offset >> NGNFS_BLOCK_SHIFT;

			if (event->res == NGNFS_BLOCK_SIZE)
				err = 0;
			else if (event->res < 0)
				err = event->res;
			else
				err = -EIO;

			ngnfs_block_end_io(ainf->nfi, bnr, data_page, err);
			put_page(data_page);

			cmm_mb(); /* load iocb fields before storing empty bit */
			set_iocb_bit(ainf, iocb, &ainf->empty_bmap);
		}
	}
}

/*
 * _submit_block has filled iocbs and marked their submit bits.  We
 * gather those iocbs and submit them to the aio context.
 */
static void submit_thread(struct thread *thr, void *arg)
{
	struct btr_aio_info *ainf = arg;
	struct iocb *iocb;
	int ret;
	int nr;

	while (!thread_should_return(thr)) {

		wait_event(&ainf->submit_waitq, uatomic_read(&ainf->submit_bmap) != 0 ||
						thread_should_return(thr));

		nr = 0;
		while ((iocb = get_and_clear_iocb_bit(ainf, &ainf->submit_bmap)))
			ainf->iocbps[nr++] = iocb;

		if (nr > 0) {
			ret = syscall(__NR_io_submit, ainf->ctx, nr, ainf->iocbps);
			assert(ret == nr);
		}
	}
}

/*
 * The caller limits the number of submitted blocks by our advertised
 * queue depth.  We find a free iocb, fill it, and hand it off to the
 * submit thread.
 */
static int btr_aio_submit_block(struct ngnfs_fs_info *nfi, void *btr_info,
				int op, u64 bnr, struct page *data_page)
{
	struct btr_aio_info *ainf = btr_info;
	struct iocb *iocb;

	iocb = get_and_clear_iocb_bit(ainf, &ainf->empty_bmap);
	BUG_ON(!iocb);

	memset(iocb, 0, sizeof(struct iocb));
	iocb->aio_data = (unsigned long)data_page;
	iocb->aio_lio_opcode = op == NGNFS_BTX_OP_WRITE ? IOCB_CMD_PWRITE : IOCB_CMD_PREAD;
	iocb->aio_fildes = ainf->dev_fd;
	iocb->aio_buf = (long)page_address(data_page);
	iocb->aio_nbytes = NGNFS_BLOCK_SIZE;
	iocb->aio_offset = bnr << NGNFS_BLOCK_SHIFT;
	iocb->aio_flags = 0;

	get_page(data_page);

	cmm_wmb(); /* store iocb fields before submit bit */
	set_iocb_bit(ainf, iocb, &ainf->submit_bmap);
	wake_up(&ainf->submit_waitq);

	return 0;
}

static int btr_aio_queue_depth(struct ngnfs_fs_info *nfi, void *btr_info)
{
	struct btr_aio_info *ainf = btr_info;

	return ainf->queue_depth;
}

static void *btr_aio_setup(struct ngnfs_fs_info *nfi, void *arg)
{
	unsigned int depth = AIO_QUEUE_DEPTH;
	struct btr_aio_info *ainf = NULL;
	char *dev_path = arg;
	int oflags;
	int ret;
	int fd;

	ainf = calloc(1, sizeof(struct btr_aio_info));
	if (!ainf) {
		ret = -ENOMEM;
		goto out;
	}

	ainf->nfi = nfi;
	ainf->queue_depth = depth;
	ainf->dev_fd = -1;
	thread_init(&ainf->submit_thr);
	thread_init(&ainf->getevents_thr);
	ainf->empty_bmap = (1UL << AIO_QUEUE_DEPTH) - 1;
	init_waitqueue_head(&ainf->submit_waitq);

	oflags = O_RDWR | O_DIRECT;
	fd = open(dev_path, oflags, O_RDWR);
	if (fd < 0 && errno == EINVAL) {
		oflags &= ~O_DIRECT;
		errno = 0;
		fd = open(dev_path, oflags, O_RDWR);
		if (fd >= 0)
			log("O_DIRECT not supported on '%s', using buffered", dev_path);
	}
	if (fd < 0) {
		ret = -errno;
		log("error opening device '%s' :" ENOF, dev_path, ENOA(-ret));
		goto out;
	}
	ainf->dev_fd = fd;

	ainf->iocbs = calloc(depth, sizeof(struct iocb));
	ainf->iocbps = calloc(depth, sizeof(struct iocb *));
	ainf->events = calloc(depth, sizeof(struct io_event));
	if (!ainf->iocbs || !ainf->iocbps || !ainf->events) {
		ret = -ENOMEM;
		log("error allocating aio ring structures: " ENOF, ENOA(-ret));
		goto out;
	}

	ret = syscall(__NR_io_setup, depth, &ainf->ctx);
	if (ret < 0) {
		ret = -errno;
		ainf->ctx = 0;
		log("io_setup nr_events=%u failed: " ENOF, depth, ENOA(-ret));
		goto out;
	}

	ret = thread_start(&ainf->submit_thr, submit_thread, ainf) ?:
	      thread_start(&ainf->getevents_thr, getevents_thread, ainf);

	ret = 0;
out:
	if (ret < 0) {
		ngnfs_btr_aio_ops.destroy(nfi, ainf);
		ainf = ERR_PTR(ret);
	}

	return ainf;
}

static void btr_aio_destroy(struct ngnfs_fs_info *nfi, void *btr_info)
{
	struct btr_aio_info *ainf = btr_info;

	if (IS_ERR_OR_NULL(ainf))
		return;

	thread_stop_indicate(&ainf->submit_thr);
	thread_stop_indicate(&ainf->getevents_thr);

	wake_up(&ainf->submit_waitq);

	/* destroying the context causes aio syscalls to return -EINVAL */
	if (ainf->ctx != 0) {
		syscall(SYS_io_destroy, ainf->ctx);
		ainf->ctx = 0;
	}

	thread_stop_wait(&ainf->submit_thr);
	thread_stop_wait(&ainf->getevents_thr);

	if (ainf->dev_fd >= 0)
		close(ainf->dev_fd);

	free(ainf->iocbs);
	free(ainf->iocbps);
	free(ainf->events);
	free(ainf);
}

struct ngnfs_block_transport_ops ngnfs_btr_aio_ops = {
	.setup = btr_aio_setup,
	.destroy = btr_aio_destroy,
	.queue_depth = btr_aio_queue_depth,
	.submit_block = btr_aio_submit_block,
};
