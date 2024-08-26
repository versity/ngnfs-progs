/* SPDX-License-Identifier: GPL-2.0 */

#include <unistd.h>
#include <errno.h>
#include <sys/uio.h>
#include <fcntl.h>

#include "shared/lk/bitops.h"
#include "shared/lk/build_bug.h"
#include "shared/lk/byteorder.h"
#include "shared/lk/err.h"
#include "shared/lk/list.h"
#include "shared/lk/math.h"
#include "shared/lk/minmax.h"
#include "shared/lk/mutex.h"
#include "shared/lk/wait.h"

#include "shared/format-trace.h"
#include "shared/thread.h"
#include "shared/trace.h"
#include "shared/urcu.h"

#define BUF_SIZE (32 * 1024)
#define NR_BUFS (1024 * 1024 / BUF_SIZE)

/*
 * userspace tracing stores tracing events in private per-thread buffer
 * pools.  As buffers fill they're handed to a writing thread.  When the
 * writing thread is done they're available for storing again.  If all
 * the buffers are writing then trace events are dropped.
 */

struct trace_info {
	struct mutex mutex;
	struct cds_list_head threads;

	int fd;
	wait_queue_head_t waitq;
	struct thread write_thr;
	struct cds_wfcq_head write_head;
	struct cds_wfcq_tail write_tail;
};

struct trace_buf {
	int refcount;
	struct cds_wfcq_node node;	/* sending to write thread */
	struct list_head head;		/* private to appending thread */
	unsigned long bits;
	void *ptr;
	size_t len;
	size_t size;
};

enum {
	TB_WRITING = 0,
};

struct trace_thread_private {
	struct cds_list_head head;
	struct list_head bufs;
	struct trace_buf *storing_buf;
};

typedef struct trace_thread_private tpriv_tls_t;
static DEFINE_URCU_TLS(tpriv_tls_t, tpriv_tls);

/* see comment above trace_setup() */
static struct trace_info *global_trinf = NULL;

static struct trace_buf *alloc_tbuf(void)
{
	struct trace_buf *tbuf;

	tbuf = malloc(sizeof(struct trace_buf) + BUF_SIZE);
	if (tbuf) {
		tbuf->refcount = 1;
		cds_wfcq_node_init(&tbuf->node);
		INIT_LIST_HEAD(&tbuf->head);
		tbuf->bits = 0;
		tbuf->ptr = (void *)(tbuf + 1);
		tbuf->len = 0;
		tbuf->size = BUF_SIZE;
	}

	return tbuf;
}

static void put_tbuf(struct trace_buf *tbuf)
{
	if (tbuf && uatomic_add_return(&tbuf->refcount, -1) == 0)
		free(tbuf);
}

static void get_tbuf(struct trace_buf *tbuf)
{
	uatomic_add(&tbuf->refcount, 1);
}

/*
 * When buffers are ready to be written they're enqueued for the writing
 * thread.  Flush can enqueue bufs while threads are still storing into
 * them so we wait for an rcu grace period for stores to drain before
 * writing.  Once we're done writing we re-initialize the buffer, clear
 * the writing bit, and drop our reference.
 */
static void trace_write_thread(struct thread *thr, void *arg)
{
	struct trace_info *trinf = arg;
	struct cds_wfcq_node *node;
	struct cds_wfcq_head head;
	struct cds_wfcq_tail tail;
	struct iovec *iov = NULL;
	struct trace_buf *tbuf;
	LIST_HEAD(list);
	ssize_t total;
	ssize_t sret;
	int iovsize = 0;
	int iovcnt;
	void *new;

	cds_wfcq_init(&head, &tail);

	/* always try to write traces before returning */
	do {
		wait_event(&trinf->waitq, !cds_wfcq_empty(&trinf->write_head, &trinf->write_tail) ||
			   thread_should_return(thr));

		__cds_wfcq_splice_nonblocking(&head, &tail, &trinf->write_head, &trinf->write_tail);

		/* wait for stores into write bufs to finish */
		synchronize_rcu();

		iovcnt = 0;
		total = 0;
		__cds_wfcq_for_each_blocking(&head, &tail, node) {
			tbuf = caa_container_of(node, struct trace_buf, node);

			if (iovcnt == iovsize) {
				iovsize += 16;
				new = reallocarray(iov, iovsize, sizeof(struct iovec));
				assert(new); /* XXX */
				iov = new;
			}

			iov[iovcnt].iov_base = tbuf->ptr;
			iov[iovcnt].iov_len = tbuf->len;
			iovcnt++;
			total += tbuf->len;
		}

		sret = writev(trinf->fd, iov, iovcnt);
		assert(sret == total); /* XXX */

		/* let destroy know we're done, usually does nothing */
		wake_up(&trinf->waitq);

		while ((node = __cds_wfcq_dequeue_nonblocking(&head, &tail))) {
			tbuf = caa_container_of(node, struct trace_buf, node);

			tbuf->len = 0;
			cds_wfcq_node_init(&tbuf->node);
			cmm_barrier(); /* re-init node before clearing writing enables use */
			clear_bit(TB_WRITING, &tbuf->bits);
			put_tbuf(tbuf);
		}
	} while (!thread_should_return(thr));

	free(iov);
}

/*
 * If we set the writing bit then grab a ref and enqueue the buf for the
 * writer.
 */
static void try_enqueue_writing(struct trace_info *trinf, struct trace_buf *tbuf)
{
	if (!test_and_set_bit(TB_WRITING, &tbuf->bits)) {
		cmm_barrier(); /* set bit before using tbuf */
		get_tbuf(tbuf);
		cds_wfcq_enqueue(&trinf->write_head, &trinf->write_tail, &tbuf->node);
		wake_up(&trinf->waitq);
	}
}

/*
 * Return a pointer for the caller to store their new trace event.  The
 * caller is holding an RCU read lock the duration of their use of the
 * pointer.
 */
void *trace_store_ptr(u16 id, size_t len)
{
	struct trace_thread_private *tpriv = &URCU_TLS(tpriv_tls);
	struct trace_info *trinf = global_trinf;
	struct ngnfs_trace_event_header *hdr;
	struct trace_buf *tbuf;
	size_t total;
	void *ptr;

	if (!trinf)
		return NULL;

	tbuf = rcu_dereference(tpriv->storing_buf);

	/* total includes header and final alignment padding */
	total = sizeof(struct ngnfs_trace_event_header) + round_up(len, sizeof(u64));

	/* see if buffer is full or flush started writing it */
	if (tbuf && ((tbuf->len + total > tbuf->size) || test_bit(TB_WRITING, &tbuf->bits))) {
		try_enqueue_writing(trinf, tbuf);

		/* move the writing buf to the end */
		list_move_tail(&tbuf->head, &tpriv->bufs);
		rcu_assign_pointer(tpriv->storing_buf, NULL);
		tbuf = NULL;
	}

	/* try to get the next buf, dropping events until the next is done writing */
	if (!tbuf) {
		tbuf = list_first_entry_or_null(&tpriv->bufs, struct trace_buf, head);
		if (!tbuf || test_bit(TB_WRITING, &tbuf->bits))
			return NULL;

		rcu_assign_pointer(tpriv->storing_buf, tbuf);
	}

	hdr = tbuf->ptr;
	hdr->id = cpu_to_le16(id);
	hdr->size = cpu_to_le16(total);

	ptr = (void *)(hdr + 1);
	tbuf->len += total;

	return ptr;
}

/*
 * Flushing trace events makes all traces visible that were in thread
 * buffers before the flush call.  We iterate over all the threads, mark
 * their current storing bufs as writing, and send them to the writer.
 *
 * We hold the RCU lock while getting buf refs via the storing_buf
 * pointer.  Exiting threads will wait for our grace period to expire
 * before freeing their tpriv that we're iterating over.
 */
void trace_flush(void)
{
	struct trace_info *trinf = global_trinf;
	struct trace_thread_private *tpriv;
	struct trace_buf *tbuf;

	rcu_read_lock();

	cds_list_for_each_entry_rcu(tpriv, &trinf->threads, head) {
		tbuf = rcu_dereference(tpriv->storing_buf);
		if (tbuf)
			try_enqueue_writing(trinf, tbuf);
	}

	rcu_read_unlock();

	/* wait for writer to finish with queued bufs */
	wait_event(&trinf->waitq, cds_wfcq_empty(&trinf->write_head, &trinf->write_tail));
}

static void put_list_bufs(struct list_head *list)
{
	struct trace_buf *tbuf;

	while ((tbuf = list_first_entry_or_null(list, struct trace_buf, head))) {
		list_del_init(&tbuf->head);
		put_tbuf(tbuf);
	}
}

/*
 * This presumes that threads are few and long lived.  We
 * unconditionally allocate large per-thread tracing buffers for all
 * threads, regardless of whether tracing is in use or not.  The urcu
 * tls helpers don't have a very useful init mechanism, so we initialize
 * newly allocated tpriv here as the first possible user in the thread.
 */
int trace_register_thread(void)
{
	struct trace_thread_private *tpriv = &URCU_TLS(tpriv_tls);
	struct trace_info *trinf = global_trinf;
	struct trace_buf *tbuf;
	int ret;
	int i;

	if (!trinf) {
		ret = 0;
		goto out;
	}

	/* urcu_tls can return null from its calloc */
	if (!tpriv) {
		ret = -ENOMEM;
		goto out;
	}

	CDS_INIT_LIST_HEAD(&tpriv->head);
	INIT_LIST_HEAD(&tpriv->bufs);
	tpriv->storing_buf = NULL;

	for (i = 0; i < NR_BUFS; i++) {
		tbuf = alloc_tbuf();
		if (!tbuf) {
			put_list_bufs(&tpriv->bufs);
			ret = -ENOMEM;
			goto out;
		}

		list_add(&tbuf->head, &tpriv->bufs);
	}

	mutex_lock(&trinf->mutex);
	cds_list_add_tail_rcu(&tpriv->head, &trinf->threads);
	mutex_unlock(&trinf->mutex);

	ret = 0;
out:
	return ret;
}

void trace_unregister_thread(void)
{
	struct trace_thread_private *tpriv = &URCU_TLS(tpriv_tls);
	struct trace_info *trinf = global_trinf;
	struct trace_buf *tbuf;

	if (!trinf || !tpriv)
		return;

	rcu_read_lock();

	/* remove our thread from the threads list */
	mutex_lock(&trinf->mutex);
	cds_list_del_rcu(&tpriv->head);
	mutex_unlock(&trinf->mutex);

	/* start write on remaining partial buf (has to be idle, we'd be storing) */
	tbuf = rcu_dereference(tpriv->storing_buf);
	if (tbuf) {
		try_enqueue_writing(trinf, tbuf);
		rcu_assign_pointer(tpriv->storing_buf, NULL);
	}
	rcu_read_unlock();

	/* wait for list/storing_buf readers (flush) to finish before putting/freeing */
	if (tbuf)
		synchronize_rcu();

	put_list_bufs(&tpriv->bufs);
	/*
	 * XXX Not sure if we're responsible for freeing tpriv or not..
	 * Seems so?
	 */
}

/*
 * Initialize the global trace state that's required for thread
 * registration.  This is done very early.
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
int trace_init(void)
{
	struct trace_info *trinf;
	int ret;

	trinf = malloc(sizeof(struct trace_info));
	if (!trinf) {
		ret = -ENOMEM;
		goto out;
	}

	mutex_init(&trinf->mutex);
	CDS_INIT_LIST_HEAD(&trinf->threads);
	trinf->fd = -1;
	init_waitqueue_head(&trinf->waitq);
	thread_init(&trinf->write_thr);
	cds_wfcq_init(&trinf->write_head, &trinf->write_tail);

	global_trinf = trinf;
	ret = 0;
out:
	return ret;
}

int trace_setup(char *trace_path)
{
	struct trace_info *trinf = global_trinf;
	int ret;

	if (!trinf)
		return 0;

	trinf->fd = open(trace_path, O_CREAT | O_WRONLY | O_APPEND, 0644);
	if (trinf->fd < 0) {
		ret = -ENOMEM;
		goto out;
	}

	/* start writing thread after trinf is initialized for its _register_thread */
	ret = thread_start(&trinf->write_thr, trace_write_thread, trinf);
out:
	if (ret < 0 && trinf->fd >= 0) {
		close(trinf->fd);
		trinf->fd = -1;
	}

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
		/* wait for writer to finish with queued bufs */
		wait_event(&trinf->waitq, cds_wfcq_empty(&trinf->write_head, &trinf->write_tail));

		/* then shut it down */
		thread_stop_indicate(&trinf->write_thr);
		wake_up(&trinf->waitq);
		thread_stop_wait(&trinf->write_thr);

		if (trinf->fd >= 0)
			close(trinf->fd);

		/*
		 * XXX I wonder if we're supposed to clean up any
		 * URCU_TLS() state that was built up.
		 */

		free(trinf);
		global_trinf = NULL;
	}
}
