/* SPDX-License-Identifier: GPL-2.0 */

#include "shared/lk/atomic.h"
#include "shared/lk/barrier.h"
#include "shared/lk/bitops.h"
#include "shared/lk/bug.h"
#include "shared/lk/build_bug.h"
#include "shared/lk/cmpxchg.h"
#include "shared/lk/err.h"
#include "shared/lk/errno.h"
#include "shared/lk/gfp.h"
#include "shared/lk/list.h"
#include "shared/lk/llist.h"
#include "shared/lk/minmax.h"
#include "shared/lk/rcupdate.h"
#include "shared/lk/rhashtable.h"
#include "shared/lk/wait.h"
#include "shared/lk/workqueue.h"

#include "shared/format-block.h"
#include "shared/fs_info.h"
#include "shared/block.h"
#include "shared/trace.h"

/*
 * This block cache coordinates block state between transaction users
 * and the underlying block transport.
 *
 * Cached blocks are tracked in an rcu hash table.  Transaction users
 * get read and write references to access and modify block contents.
 * Read references are shared with readers and a single write reference
 * is exclusive.
 *
 * Write references leave behind dirty blocks, which are later submitted
 * to the block transport for writing.  Read references are allowed
 * while blocks are being written but writes are excluded.
 *
 * There are some very simple thresholds on the number of dirty blocks
 * at which start background flushing writes or limit the total number
 * of dirty blocks.
 *
 * Today there are no atomic write transactions.  Dirty blocks are added
 * to a flush list and are written, and can fail, independently.  This
 * will change as the network protocol adds support for distributed
 * transactions.  The cache will need to track atomic units of dirty
 * blocks and hand them to the transport which can describe the group in
 * write messages sent over the wire.
 *
 * Without the cache coherency network protocol this is missing a huge
 * portion of its functionality.  It will need to track granted access
 * when satisfying reference requests, and will need to respond to
 * access revocation messages over the wire.
 *
 * When implementing invalidations and atomic writes, there's an
 * irritating deadlock to address.  Transactions are tracking their
 * block references so they can use trylock semantics to acquire
 * references (and request access over the network) out of order and be
 * asked to retry in order when they hit a potential deadlock. Write
 * references might pin entire existing dirty atomic block sets, not
 * just individual blocks.  This effectively means that transactions are
 * also holding references to all the blocks in the dirty set, without
 * knowing it.  They could block on a reference in order with their
 * transaction blocks but out of order with respect to the dirty blocks
 * that are pinned.  (One fix might be to have invalidations wake
 * blocked reference acquisitions that pin the set.  We'll see what
 * seems least awful.)
 */

/*
 * Callers will wait to enter their write transaction until the number
 * of dirty blocks falls below this limit.  They can exceed the limit
 * while they're dirtying blocks in their transaction.
 */
#define DIRTY_LIMIT	1024

/*
 * Background flushing will start when the number of dirty blocks exceeds this
 * threshold.
 */
#define FLUSH_THRESH	256

/*
 * Stages in the block processing pipeline gather inputs from multiple
 * producers on a lockless list for a single work consumer to manage
 * with a private list_head list.
 */
struct block_work_list {
	struct llist_head llist;
	struct list_head list;
	struct workqueue_struct *wq;
	struct work_struct work;
};

struct ngnfs_block_info {
	struct ngnfs_fs_info *nfi;
	struct rhashtable ht;

	int queue_depth;
	atomic_t nr_dirty;
	atomic_t nr_flushing;
	atomic_t nr_submitted;
	atomic_t sync_waiters;

	struct block_work_list flush;
	struct block_work_list submit;
	struct block_work_list clean;

	struct ngnfs_block_transport_ops *btr_ops;
	void *btr_info;

	wait_queue_head_t waitq;
};

struct ngnfs_block {
	atomic_t refcount;
	atomic_t rw_count;
	struct rcu_head rcu;
	struct rhash_head rhead;
	struct llist_node dirty_llnode;
	struct list_head dirty_head;
	struct llist_node submit_llnode;
	struct list_head submit_head;
	wait_queue_head_t waitq;
	unsigned long bits; /* BL_ block bits */
	int error;
	struct page *page;
	u64 bnr;
};

enum {
	/*
	 * An IO is in flight.  Set when added to the submit list and
	 * cleared by the end_io callback.
	 */
	BL_IO_PENDING = 0,
	/*
	 * Set as reads complete and indicates that the current contents
	 * are in sync with the persistent block.  Readers and writers
	 * can use block references once this is set.
	 */
	BL_UPTODATE,
	/*
	 * IO failed.  The block will be removed from the cache once all
	 * references get a chance to return the error and put their
	 * reference.
	 */
	BL_ERROR,
	/*
	 * The block is dirty and must be written before it can be
	 * freed.  It makes its way through the flush, submit, and clean
	 * work.
	 */
	BL_DIRTY,
	/*
	 * This block only exists to track a sync waiters ordering with
	 * dirty blocks in the flush and clean lists.
	 */
	BL_SYNC_WAITER,
};

/* declaring these here so that we can have their wake condition along side their work */
static void try_queue_flush_work(struct ngnfs_block_info *blinf);
static void try_queue_submit_work(struct ngnfs_block_info *blinf);
static void queue_clean_work(struct ngnfs_block_info *blinf);

static void init_block_work_list(struct block_work_list *worklist, work_func_t func)
{
	init_llist_head(&worklist->llist);
	INIT_LIST_HEAD(&worklist->list);
	worklist->wq = NULL;
	INIT_WORK(&worklist->work, func);
}

static void destroy_block_work_list(struct block_work_list *worklist)
{
	if (worklist->wq)
		destroy_workqueue(worklist->wq);
}

static void free_block(struct ngnfs_block *bl)
{
	if (!IS_ERR_OR_NULL(bl)) {
		BUG_ON(waitqueue_active(&bl->waitq));

		if (bl->page)
			put_page(bl->page);
		kfree(bl);
	}
}

static struct ngnfs_block *alloc_block(u64 bnr, bool with_page)
{
	struct ngnfs_block *bl;

	/* should know how to alloc sub pages */
	BUILD_BUG_ON(NGNFS_BLOCK_SIZE < PAGE_SIZE);

	bl = kzalloc(sizeof(struct ngnfs_block), GFP_NOFS);
	if (bl) {
		atomic_set(&bl->refcount, 1);
		init_llist_node(&bl->dirty_llnode);
		init_llist_node(&bl->submit_llnode);
		INIT_LIST_HEAD(&bl->submit_head);
		init_waitqueue_head(&bl->waitq);

		if (with_page)
			bl->page = alloc_page(GFP_NOFS);
		bl->bnr = bnr;
	}

	if (!bl || (with_page && !bl->page)) {
		free_block(bl);
		bl = ERR_PTR(-ENOMEM);
	}

	return bl;
}

static void free_block_rcu(struct rcu_head *rcu)
{
	struct ngnfs_block *bl = container_of(rcu, struct ngnfs_block, rcu);

	free_block(bl);
}

static void get_block(struct ngnfs_block *bl)
{
	int now = atomic_inc_return(&bl->refcount);

	BUG_ON(now <= 0);
}

static void put_block(struct ngnfs_block *bl)
{
	int now = 0;

	if (!IS_ERR_OR_NULL(bl)) {
		now = atomic_dec_return(&bl->refcount);
		if (now == 0)
			call_rcu(&bl->rcu, free_block_rcu);
		else
			BUG_ON(now < 0);
	}
}

/*
 * Get read/write rw_count.  Concurrent readers increment the rw_count.
 * An exclusive writer sets the rw_count from 0 to -1.  A caller's
 * single read reference can be directly converted to a writer.
 */
static bool get_read_write(struct ngnfs_block *bl, nbf_t nbf)
{
	BUG_ON(atomic_read(&bl->rw_count) < -1);
	BUG_ON((nbf & NBF_CONVERT_WRITE) && atomic_read(&bl->rw_count) < 1);

	if (nbf & NBF_CONVERT_WRITE)
		return atomic_cmpxchg(&bl->rw_count, 1, -1) == 1;
	else if (nbf & NBF_WRITE)
		return atomic_cmpxchg(&bl->rw_count, 0, -1) == 0;
	else
		return atomic_inc_unless_negative(&bl->rw_count);
}

static void put_read_write(struct ngnfs_block *bl, nbf_t nbf)
{
	int now;

	if (nbf & NBF_WRITE) {
		now = atomic_inc_return(&bl->rw_count);
		BUG_ON(now != 0);
	} else {
		now = atomic_dec_return(&bl->rw_count);
		BUG_ON(now < 0);
	}
}

/*
 * We use an atomic to record any io errors for the tasks that are in
 * sync.  We use the low bit to indicate error and assume that we'll
 * never have enough waiters to overflow.
 */
#define SYNC_WAITERS_ERR 1
#define SYNC_WAITERS_INC 2

static void sync_waiters_inc(struct ngnfs_block_info *blinf)
{
	atomic_add(SYNC_WAITERS_INC, &blinf->sync_waiters);
}

static void sync_waiters_set_error(struct ngnfs_block_info *blinf)
{
	int old;

	do {
		old = atomic_read(&blinf->sync_waiters);
	} while ((old >= SYNC_WAITERS_INC) && !(old & SYNC_WAITERS_ERR) &&
		 (atomic_cmpxchg(&blinf->sync_waiters, old, old | SYNC_WAITERS_ERR) != old));
}

static bool sync_waiters_has_error(struct ngnfs_block_info *blinf)
{
	return !!(atomic_read(&blinf->sync_waiters) & SYNC_WAITERS_ERR);
}

/*
 * Decrement the caller's previous increment of sync_waiters, returning
 * -EIO if there was an error while they were waiting, and clearing the
 * error if they were the last waiter.
 */
static int sync_waiters_dec_error(struct ngnfs_block_info *blinf)
{
	int ret = 0;
	int old;
	int new;

	do {
		old = atomic_read(&blinf->sync_waiters);
		ret = (old & SYNC_WAITERS_ERR) ? -EIO : 0;
		new = old - SYNC_WAITERS_INC;
		if (new == SYNC_WAITERS_ERR)
			new = 0;

	} while (atomic_cmpxchg(&blinf->sync_waiters, old, new) != old);

	return ret;
}

static const struct rhashtable_params ngnfs_block_ht_params = {
        .head_offset = offsetof(struct ngnfs_block, rhead),
        .key_offset = offsetof(struct ngnfs_block, bnr),
        .key_len = sizeof_field(struct ngnfs_block, bnr),
};

static struct ngnfs_block *lookup_block(struct ngnfs_block_info *blinf, u64 bnr)
{
	struct ngnfs_block *bl;

	rcu_read_lock();
	bl = rhashtable_lookup(&blinf->ht, &bnr, ngnfs_block_ht_params);
	if (bl)
		get_block(bl);
	rcu_read_unlock();

	return bl;
}

/*
 * Returns a block with a reference held or an ERR_PTR on allocation
 * failure or lookup that won't allocate.
 */
static struct ngnfs_block *lookup_or_alloc_block(struct ngnfs_block_info *blinf, u64 bnr)
{
	struct ngnfs_block *found;
	struct ngnfs_block *bl;

	bl = lookup_block(blinf, bnr);
	if (!bl) {
		bl = alloc_block(bnr, true);
		if (!IS_ERR(bl)) {
			get_block(bl);
			rcu_read_lock();
			found = rhashtable_lookup_get_insert_fast(&blinf->ht, &bl->rhead,
								  ngnfs_block_ht_params);
			if (found) {
				put_block(bl);
				put_block(bl);
				bl = found;
				get_block(bl);
			}
			rcu_read_unlock();
		}
	}

	return bl;
}

/*
 * An incoming data_page ref is only used for reads. Writes always
 * manage source page that contains their written contents.  If a read
 * data_page is provided then we swap it in to place and drop the old
 * (unused) block page.
 */
void ngnfs_block_end_io(struct ngnfs_fs_info *nfi, u64 bnr, struct page *data_page, int err)
{
	struct ngnfs_block_info *blinf = nfi->block_info;
	struct ngnfs_block *bl;
	bool is_write;
	int nr_dirty;

	/* XXX describe trying page granular pinning */

	bl = lookup_block(blinf, bnr);
	assert(!IS_ERR_OR_NULL(bl)); /* not supporting this failure yet */
	is_write = !!test_bit(BL_DIRTY, &bl->bits);

	if (err) {
		if (!test_and_set_bit(BL_ERROR, &bl->bits))
			bl->error = err;
		if (is_write)
			sync_waiters_set_error(blinf);
	}

	if (is_write) {
		/* updating accounting here, clean work puts reader */
		clear_bit(BL_DIRTY, &bl->bits);
		nr_dirty = atomic_dec_return(&blinf->nr_dirty);
	} else {
		if (!test_bit(BL_ERROR, &bl->bits))
			set_bit(BL_UPTODATE, &bl->bits);

		if (data_page) {
			/* this means that _block_buf() will change, callers beware */
			if (bl->page)
				put_page(bl->page);
			bl->page = data_page;
			get_page(bl->page);
		}
	}

	clear_bit(BL_IO_PENDING, &bl->bits);
	atomic_dec(&blinf->nr_submitted);

	barrier(); /* RELEASE: all block updates visible before we queue/wake */

	wake_up(&bl->waitq);
	put_block(bl);

	if (is_write) {
		try_queue_flush_work(blinf);
		queue_clean_work(blinf);
		if (nr_dirty < DIRTY_LIMIT && waitqueue_active(&blinf->waitq))
			wake_up(&blinf->waitq);
	}
}

/*
 * Callers are gathering items that were concurrently prepended to a
 * lockless list and are putting them on their private list_head list.
 * We preserve list order (for sync especially) so we walk the llist
 * lifo and construct a private fifo that is then spliced onto the end
 * of the caller's existing list.
 *
 * The lockless list is destroyed and its nodes are re-initialized as we
 * go.  The caller is only using the nodes to get the item on their
 * private list.  They can then add the items to other lockless lists as
 * needed.
 */
static void del_all_reverse_add_tail(struct list_head *list, struct llist_head *llist,
				     ssize_t offset)
{
	struct llist_node *first;
	struct llist_node *node;
	struct llist_node *n;
	struct list_head *head;
	LIST_HEAD(reverse);

	first = llist_del_all(llist);
	if (first) {
		llist_for_each_safe(node, n, first) {
			head = (void *)node + offset;
			init_llist_node(node);
			list_add(head, &reverse);
		}
		list_splice_tail(&reverse, list);
	}
}

/*
 * XXX barriers?
 */
static void try_queue_submit_work(struct ngnfs_block_info *blinf)
{
	if ((!list_empty(&blinf->submit.list) || !llist_empty(&blinf->submit.llist)) &&
	    (atomic_read(&blinf->nr_submitted) < blinf->queue_depth))
		queue_work(blinf->submit.wq, &blinf->submit.work);
}

/*
 * The submit work is responsible for keeping the transport's queue
 * depth full.
 */
static void ngnfs_block_submit_work(struct work_struct *work)
{
	struct ngnfs_block_info *blinf = container_of(work, struct ngnfs_block_info, submit.work);
	struct ngnfs_fs_info *nfi = blinf->nfi;
	struct ngnfs_block *tmp;
	struct ngnfs_block *bl;
	int submitted;
	int space;
	int ret;
	int op;

	del_all_reverse_add_tail(&blinf->submit.list, &blinf->submit.llist,
				 offsetof(struct ngnfs_block, submit_head) -
				 offsetof(struct ngnfs_block, submit_llnode));

	space = blinf->queue_depth - atomic_read(&blinf->nr_submitted);
	submitted = 0;

	list_for_each_entry_safe(bl, tmp, &blinf->submit.list, submit_head) {
		if (submitted == space)
			break;

		list_del_init(&bl->submit_head);

		/* XXX _GET_WRITE isn't implemented */
		op = test_bit(BL_DIRTY, &bl->bits) ? NGNFS_BTX_OP_WRITE : NGNFS_BTX_OP_GET_READ;

		ret = blinf->btr_ops->submit_block(nfi, blinf->btr_info, op, bl->bnr, bl->page);
		BUG_ON(ret != 0);

		submitted++;
	}

	if (submitted)
		atomic_add(submitted, &blinf->nr_submitted);
}

/*
 * XXX barriers?
 */
static int should_flush(struct ngnfs_block_info *blinf)
{
	int dirty = atomic_read(&blinf->nr_dirty);
	int flushing = atomic_read(&blinf->nr_flushing);
	int depth = blinf->queue_depth * 2;

	if (dirty > FLUSH_THRESH && flushing < depth)
		return depth - flushing;

	if (atomic_read(&blinf->sync_waiters) >= SYNC_WAITERS_INC)
		return dirty - flushing;

	return 0;
}

static void try_queue_flush_work(struct ngnfs_block_info *blinf)
{
	if (should_flush(blinf))
		queue_work(blinf->flush.wq, &blinf->flush.work);
}

/*
 * The flush work is responsible for submitting write IO for dirty
 * blocks on the flush list.  Today the blocks are passed straight
 * through to the submit work.
 *
 * As write transactions are implemented this will be responsible for
 * grouping the blocks into transaction fragments and for resending if
 * the maps change.
 */
static void ngnfs_block_flush_work(struct work_struct *work)
{
	struct ngnfs_block_info *blinf = container_of(work, struct ngnfs_block_info,
						      flush.work);
	struct ngnfs_block *tmp;
	struct ngnfs_block *bl;
	bool submitted = false;
	int should;

	/* always gather dirtied sets from llist for iteration */
	del_all_reverse_add_tail(&blinf->flush.list, &blinf->flush.llist,
				 offsetof(struct ngnfs_block, dirty_head) -
				 offsetof(struct ngnfs_block, dirty_llnode));

	should = should_flush(blinf);
	BUG_ON(should < 0);

	list_for_each_entry_safe(bl, tmp, &blinf->flush.list, dirty_head) {
		if (test_bit(BL_SYNC_WAITER, &bl->bits)) {
			list_del_init(&bl->dirty_head);
			llist_add(&bl->dirty_llnode, &blinf->clean.llist);
			queue_clean_work(blinf);
			continue;
		}

		if (should-- == 0)
			break;

		/* get reader while flushing to exclude writers */
		if (!get_read_write(bl, NBF_READ))
			break;

		list_del_init(&bl->dirty_head);
		set_bit(BL_IO_PENDING, &bl->bits);
		get_block(bl);
		atomic_inc(&blinf->nr_flushing);

		barrier(); /* release: order updates before visible on lists */

		llist_add(&bl->dirty_llnode, &blinf->clean.llist);
		llist_add(&bl->submit_llnode, &blinf->submit.llist);

		submitted = true;
	}

	if (submitted)
		try_queue_submit_work(blinf);
}

static void queue_clean_work(struct ngnfs_block_info *blinf)
{
	queue_work(blinf->clean.wq, &blinf->clean.work);
}

/*
 * The clean work's only job is to walk the clean list and remove blocks
 * whose write IO has completed.  We defer this after end_io so that we
 * can put sync waiters in the flush->clean list and wake them once all
 * the write IO they're waiting for is complete.
 */
static void ngnfs_block_clean_work(struct work_struct *work)
{
	struct ngnfs_block_info *blinf = container_of(work, struct ngnfs_block_info, clean.work);
	struct ngnfs_block *tmp;
	struct ngnfs_block *bl;
	bool cleaned = false;

	del_all_reverse_add_tail(&blinf->clean.list, &blinf->clean.llist,
				 offsetof(struct ngnfs_block, dirty_head) -
				 offsetof(struct ngnfs_block, dirty_llnode));

	list_for_each_entry_safe(bl, tmp, &blinf->clean.list, dirty_head) {

		if (test_bit(BL_IO_PENDING, &bl->bits))
			break;

		list_del_init(&bl->dirty_head);

		if (test_bit(BL_SYNC_WAITER, &bl->bits)) {
			clear_bit(BL_SYNC_WAITER, &bl->bits);
		} else {
			atomic_dec(&blinf->nr_flushing);
			put_read_write(bl, NBF_READ);
			cleaned = true;
		}

		wake_up(&bl->waitq);
		put_block(bl);
	}

	if (cleaned)
		try_queue_flush_work(blinf);
}

static bool bad_nbf(nbf_t nbf)
{
	return ((nbf & NBF_READ) && (nbf & NBF_WRITE)) ||
	       ((nbf & (NBF_NEW | NBF_NODIRTY | NBF_CONVERT_WRITE)) && !(nbf & NBF_WRITE));
}

/*
 * Acquire a reference to a cached block.  The behaviour of the
 * reference is controlled by the flags as documented at the nbf_t
 * definition.  Successfully acquired references must later be released
 * by calling _put().
 */
struct ngnfs_block *ngnfs_block_get(struct ngnfs_fs_info *nfi, u64 bnr, nbf_t nbf)
{
	struct ngnfs_block_info *blinf = nfi->block_info;
	struct ngnfs_block *bl = NULL;
	int ret;

	if (WARN_ON_ONCE(bad_nbf(nbf))) {
		ret = -EINVAL;
		goto out;
	}

	bl = lookup_or_alloc_block(blinf, bnr);
	if (IS_ERR(bl)) {
		ret = PTR_ERR(bl);
		goto out;
	}

	if (!get_read_write(bl, nbf)) {
		if (nbf & (NBF_TRY | NBF_CONVERT_WRITE)) {
			ret = -EDEADLK;
			goto out;
		}
		wait_event(&bl->waitq, get_read_write(bl, nbf));
	}

	/* new is used by writers to set uptodate without reading */
	if (nbf & NBF_NEW) {
		wait_event(&bl->waitq, !test_bit(BL_IO_PENDING, &bl->bits));
		memset(ngnfs_block_buf(bl), 0, NGNFS_BLOCK_SIZE); /* XXX caller's job? */
		set_bit(BL_UPTODATE, &bl->bits);
		clear_bit(BL_ERROR, &bl->bits);
		bl->error = 0;
	}

	if (!test_bit(BL_UPTODATE, &bl->bits)) {
		if (!test_and_set_bit(BL_IO_PENDING, &bl->bits)) {
			get_block(bl); /* presence on submit lists before hitting transport */
			llist_add(&bl->submit_llnode, &blinf->submit.llist);
			try_queue_submit_work(blinf);
		}

		wait_event(&bl->waitq, !test_bit(BL_IO_PENDING, &bl->bits));
	}

	if (test_bit(BL_ERROR, &bl->bits)) {
		ret = bl->error;
		put_read_write(bl, nbf);
	} else {
		ret = 0;
	}

out:
	if (ret < 0)  {
		put_block(bl);
		bl = ERR_PTR(ret);
	}

	return bl;
}

/*
 * Release a block reference.  The nbf flags must start with matching
 * the mode of the previously successful _get call but can add
 * additional flags that change behaviour.  (We might want a more robust
 * "holder" struct that flags then modify.)
 */
void ngnfs_block_put(struct ngnfs_fs_info *nfi, struct ngnfs_block *bl, nbf_t nbf)
{
	struct ngnfs_block_info *blinf = nfi->block_info;

	if ((nbf & NBF_WRITE) && !test_bit(BL_DIRTY, &bl->bits) && !(nbf & NBF_NODIRTY)) {
		set_bit(BL_DIRTY, &bl->bits);
		get_block(bl);
		atomic_inc(&blinf->nr_dirty);
		/* XXX barrier? */
		llist_add(&bl->dirty_llnode, &blinf->flush.llist);
		try_queue_flush_work(blinf);
	}

	put_read_write(bl, nbf);
	wake_up(&bl->waitq);
	put_block(bl);
}

void *ngnfs_block_buf(struct ngnfs_block *bl)
{
	return page_address(bl->page);
}

struct page *ngnfs_block_page(struct ngnfs_block *bl)
{
	return bl->page;
}

/*
 * Wait until the number of dirty blocks is under the hard limit.
 * There's no measures taken to avoid thundering herds, and once writers
 * proceed past this wait they can each dirty their full transaction's
 * worth of blocks.
 */
void ngnfs_block_dirty_limit_wait(struct ngnfs_fs_info *nfi)
{
	struct ngnfs_block_info *blinf = nfi->block_info;

	/* XXX probably interruptible, io errors won't clear dirty */
	wait_event(&blinf->waitq, atomic_read(&blinf->nr_dirty) < DIRTY_LIMIT);
}

/*
 * This sync blocks until previously dirty blocks are flushed and
 * written out successfully.  It captures blocks that were dirtied by
 * _put calls that returned before this _sync call started.
 *
 * The error tracking is pretty clumsy.  The first write error seen
 * while any sync is waiting is returned to all waiting syncs.  It's
 * goofy, but easy.
 *
 * The ordering between waiters and dirty blocks is achieved by having
 * each waiter insert a fake block into the flush list.  It'll be woken
 * once the clean work finds it in the clean list after all previously
 * dirty blocks.
 */
int ngnfs_block_sync(struct ngnfs_fs_info *nfi)
{
	struct ngnfs_block_info *blinf = nfi->block_info;
	struct ngnfs_block *bl;

	bl = alloc_block(0, false);
	if (IS_ERR(bl))
		return PTR_ERR(bl);

	set_bit(BL_SYNC_WAITER, &bl->bits);

	sync_waiters_inc(blinf);

	llist_add(&bl->dirty_llnode, &blinf->flush.llist);
	try_queue_flush_work(blinf);

	wait_event(&bl->waitq, sync_waiters_has_error(blinf) ||
			       !(test_bit(BL_SYNC_WAITER, &bl->bits)));
	put_block(bl);

	return sync_waiters_dec_error(blinf);
}

int ngnfs_block_setup(struct ngnfs_fs_info *nfi, struct ngnfs_block_transport_ops *btr_ops,
		      void *btr_setup_arg)
{
	struct ngnfs_block_info *blinf;
	int ret;

	blinf = kzalloc(sizeof(struct ngnfs_block_info), GFP_KERNEL);
	if (!blinf) {
		ret = -ENOMEM;
		goto out;
	}

	blinf->nfi = nfi;
	atomic_set(&blinf->nr_dirty, 0);
	atomic_set(&blinf->nr_flushing, 0);
	atomic_set(&blinf->nr_submitted, 0);
	atomic_set(&blinf->sync_waiters, 0);
	init_block_work_list(&blinf->flush, ngnfs_block_flush_work);
	init_block_work_list(&blinf->submit, ngnfs_block_submit_work);
	init_block_work_list(&blinf->clean, ngnfs_block_clean_work);
	blinf->btr_ops = btr_ops;
	init_waitqueue_head(&blinf->waitq);

	ret = rhashtable_init(&blinf->ht, &ngnfs_block_ht_params);
	if (ret < 0)
		goto out_free;

	/* XXX use fs identifier in name */
	blinf->flush.wq = create_singlethread_workqueue("ngnfs-flush");
	blinf->submit.wq = create_singlethread_workqueue("ngnfs-submit");
	blinf->clean.wq = create_singlethread_workqueue("ngnfs-clean");
	if (!blinf->flush.wq || !blinf->submit.wq || !blinf->clean.wq) {
		ret = -ENOMEM;
		goto out_destroy;
	}

	if (blinf->btr_ops->setup) {
		blinf->btr_info = blinf->btr_ops->setup(nfi, btr_setup_arg);
		if (IS_ERR(blinf->btr_info)) {
			ret = PTR_ERR(blinf->btr_info);
			goto out_destroy;
		}
	}

	blinf->queue_depth = blinf->btr_ops->queue_depth(nfi, blinf->btr_info);
	nfi->block_info = blinf;
	ret = 0;
	goto out;

out_destroy:
	destroy_block_work_list(&blinf->flush);
	destroy_block_work_list(&blinf->submit);
	destroy_block_work_list(&blinf->clean);
	rhashtable_destroy(&blinf->ht);
out_free:
	kfree(blinf);
out:
	return ret;
}

/*
 * The rhashtable caller is destroying the hash table as it calls us, we don't
 * have to remove blocks from the table.
 */
static void free_ht_block(void *ptr, void *arg)
{
	struct ngnfs_block *bl = ptr;

	put_block(bl);
}

/*
 * Once we're destroying we should have no more callers who would queue
 * work.  We shutdown the block transport to stop further IO completion
 * which could queue work.
 */
void ngnfs_block_destroy(struct ngnfs_fs_info *nfi)
{
	struct ngnfs_block_info *blinf = nfi->block_info;

	if (blinf) {
		if (blinf->btr_ops->shutdown)
			blinf->btr_ops->shutdown(nfi, blinf->btr_info);

		/* any queued work is drained before destruction */
		destroy_block_work_list(&blinf->flush);
		destroy_block_work_list(&blinf->submit);
		destroy_block_work_list(&blinf->clean);

		if (blinf->btr_ops->destroy)
			blinf->btr_ops->destroy(nfi, blinf->btr_info);
		rhashtable_free_and_destroy(&blinf->ht, free_ht_block, blinf);
		kfree(blinf);
		nfi->block_info = NULL;
	}
}
