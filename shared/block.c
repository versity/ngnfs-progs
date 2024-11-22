/* SPDX-License-Identifier: GPL-2.0 */

/*
 * This block cache layer is responsible for providing read and write
 * access to blocks which are transferred over an underlying transport
 * -- typically either over the network or over a local block IO
 * transport.
 *
 * Cached blocks are indexed in a hash table with block lifetimes
 * governed by RCU.  Clean cached blocks can be reclaimed at any time.
 *
 * Callers dirty blocks in dependent groups.  We maintain this grouping
 * by tracking dirty blocks in sets.  Dirty sets can be merged if their
 * blocks are modified in one dirty operation.
 *
 * Writeback is performed in terms of sets, in the order that they were
 * initially dirtied.  Background memory pressure or explicit cache sync
 * operations can trigger writeback.
 *
 * XXX:
 *  - This doesn't yet support exclusive read and write references.
 *    Some callers won't have serialization of operations do we'll be
 *    implementing serialization down in the blocks.  If it works out,
 *    anyway.
 *  - We'll need some form of shrinking.  We'll want some form of access
 *    marking so that we don't throw away recently used blocks without
 *    also creating a ton of contention.
 */

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
#include "shared/lk/processor.h"
#include "shared/lk/rcupdate.h"
#include "shared/lk/rhashtable.h"
#include "shared/lk/wait.h"
#include "shared/lk/workqueue.h"

#include "shared/format-block.h"
#include "shared/fs_info.h"
#include "shared/block.h"
#include "shared/urcu.h"
#include "shared/trace.h"

/*
 * Tasks stop dirtying additional blocks once this many blocks are
 * dirty.  Sets have to complete writeback and mark their blocks clean
 * before more blocks can be dirtied.
 */
#define DIRTY_LIMIT	1024

/*
 * Writeback will start when the number of dirty blocks exceeds this
 * threshold.
 */
#define WRITEBACK_THRESH	256

/*
 * The maximum number of blocks in a dirty set.  This is effectively
 * also the limit of the number of blocks that can be modified in a
 * transaction.  Attempts to add more dirty blocks to a full set will
 * write it out.
 */
#define SET_LIMIT	64

struct ngnfs_block_info {
	struct rhashtable ht;

	int queue_depth;
	atomic_t nr_dirty;
	atomic_t nr_writeback;
	atomic_t nr_submitted;
	atomic_t sync_waiters;

	atomic64_t dirty_seq;
	atomic64_t writeback_seq;
	atomic64_t sync_seq;

	struct llist_head submit_llist;
	struct list_head submit_list;
	struct llist_head writeback_llist;
	struct list_head writeback_list;

	struct ngnfs_fs_info *nfi;
	struct workqueue_struct *wq;
	struct work_struct submit_work;
	struct work_struct writeback_work;

	struct ngnfs_block_transport_ops *btr_ops;
	void *btr_info;

	wait_queue_head_t waitq;
};

/*
 * Dirty blocks are grouped into sets of blocks whose modifications
 * depend on each other and which must all be written atomically.   The
 * sets have a maximum number of blocks and will be merged when
 * an operation modifies blocks in different sets.
 */
struct ngnfs_block_set {
	struct rcu_head rcu;
	atomic_t refcount;
	atomic_t submitted_blocks;
	struct llist_node writeback_llnode;
	struct list_head writeback_head;
	struct list_head block_list;
	wait_queue_head_t waitq;
	u64 dirty_seq;
	unsigned long bits; /* SET_ set bits */
	unsigned size;
};

enum {
	/*
	 * The exclusive setter of the bit is dirtying the blocks in the
	 * set and possibly merging it with other sets.  Other dirtying
	 * or writeback attempts will wait.
	 */
	SET_DIRTYING = 0,
	/*
	 * The set contains modified dirty blocks.  It's dirty blocks
	 * are contributing to nr_dirty and its writeback seq has been
	 * set by addition to the writeback llist.
	 */
	SET_DIRTY,
	/*
	 * The blocks in the set are under IO.  Dirtying attempts will wait.
	 */
	SET_WRITEBACK,
};

struct ngnfs_block {
	struct ngnfs_block_set *set;
	atomic_t refcount;
	struct rcu_head rcu;
	struct rhash_head rhead;
	struct llist_node submit_llnode;
	struct list_head submit_head;
	struct list_head set_head;
	wait_queue_head_t waitq;
	unsigned long bits; /* BL_ block bits */
	int error;
	struct page *page;
	u64 bnr;
};

enum {
	/*
	 * Set between when a block is queued for reading IO and when IO
	 * finishes.
	 */
	BL_READING = 0,
	/*
	 * Set as reads complete and indicates that the current contents
	 * are in sync with the persistent block.  Readers and writers
	 * can use block references once this is set.
	 */
	BL_UPTODATE,
	/*
	 * IO failed.  Will be removed from the cache once all
	 * references get a chance to return the error and put their
	 * reference.
	 */
	BL_ERROR,
	/*
	 * The block is present in a set of dirty blocks.
	 */
	BL_DIRTY,
};

/* declaring these as we want their wake logic along side the work logic */
static void try_queue_submit_work(struct ngnfs_block_info *blinf);
static void try_queue_writeback_work(struct ngnfs_block_info *blinf);

static inline void clear_bit_and_wake_up(int nr, unsigned long *bits, wait_queue_head_t *wq)
{
	if (test_and_clear_bit(nr, bits)) {
		smp_mb(); /* store clear before loading waitq */
		if (waitqueue_active(wq))
			wake_up(wq);
	}
}

static void free_block(struct ngnfs_block *bl)
{
	if (!IS_ERR_OR_NULL(bl)) {
		BUG_ON(!list_empty(&bl->set_head));
		BUG_ON(waitqueue_active(&bl->waitq));

		if (bl->page)
			put_page(bl->page);
		kfree(bl);
	}
}

static struct ngnfs_block *alloc_block(u64 bnr)
{
	struct ngnfs_block *bl;

	/* should know how to alloc sub pages */
	BUILD_BUG_ON(NGNFS_BLOCK_SIZE < PAGE_SIZE);

	bl = kzalloc(sizeof(struct ngnfs_block), GFP_NOFS);
	if (bl) {
		atomic_set(&bl->refcount, 1);
		init_llist_node(&bl->submit_llnode);
		INIT_LIST_HEAD(&bl->submit_head);
		INIT_LIST_HEAD(&bl->set_head);
		init_waitqueue_head(&bl->waitq);

		bl->page = alloc_page(GFP_NOFS);
		bl->bnr = bnr;
	}

	if (!bl || !bl->page) {
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
	atomic_inc(&bl->refcount);
}

static void put_block(struct ngnfs_block *bl)
{
	if (!IS_ERR_OR_NULL(bl) && atomic_dec_return(&bl->refcount) == 0)
		call_rcu(&bl->rcu, free_block_rcu);
}

static void get_set(struct ngnfs_block_set *set)
{
	atomic_inc(&set->refcount);
}

static void put_set(struct ngnfs_block_set *set)
{
	if (!IS_ERR_OR_NULL(set) && atomic_dec_return(&set->refcount) == 0) {
		BUG_ON(!list_empty(&set->block_list));
		BUG_ON(set->size != 0);
		kfree_rcu(&set->rcu);
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
	} while (old >= SYNC_WAITERS_INC &&
		 (atomic_cmpxchg(&blinf->sync_waiters, old, old | SYNC_WAITERS_ERR) != old));
}

static bool sync_waiters_has_error(struct ngnfs_block_info *blinf)
{
	return !!(atomic_read(&blinf->sync_waiters) & SYNC_WAITERS_ERR);
}

/*
 * Decrement the callers previous increment of sync_waiters, returning
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

/*
 * We have sequence numbers that record the order of sets being dirtied
 * and starting writeback.  We trigger writeback on behalf of the
 * caller's sync if their seqs haven't started writeback yet.  We then
 * wait for them to start and for there to be no more blocks in flight.
 *
 * We use a sort of latched sync error state.  While there are sync
 * waiters we record IO errors for all the waiters. Only once all the
 * waiters leave is the error cleared.
 *
 * Neither the livelocking of sync by new blocks entering writeback nor
 * the broadcasting of errors to all waiters are great, but it makes for
 * a simple initial implementation.
 */
static int sync_up_to_seq(struct ngnfs_block_info *blinf, u64 seq)
{
	u64 sync_seq;

	sync_waiters_inc(blinf);

	do {
		sync_seq = atomic64_read(&blinf->sync_seq);
	} while (seq > sync_seq &&
		 (atomic64_cmpxchg(&blinf->sync_seq, sync_seq, seq) != sync_seq));

	if (seq > sync_seq)
		try_queue_writeback_work(blinf);

	trace_ngnfs_sync_begin(seq);

	wait_event(&blinf->waitq, sync_waiters_has_error(blinf) ||
		   (atomic64_read(&blinf->writeback_seq) >= seq &&
		    atomic_read(&blinf->nr_writeback) == 0));

	return sync_waiters_dec_error(blinf);
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
		bl = alloc_block(bnr);
		if (!IS_ERR(bl)) {
			/* XXX can this found == bl? */
			rcu_read_lock();
			found = rhashtable_lookup_get_insert_fast(&blinf->ht, &bl->rhead,
								  ngnfs_block_ht_params);
			if (found) {
				put_block(bl);
				get_block(found);
				bl = found;
			} else {
				get_block(bl);
			}
			rcu_read_unlock();
		}
	}

	return bl;
}

/*
 * If data_page is provided then it is a new page that the io transport
 * allocated to store an incoming read.  We swap it in to place and drop
 * the old (unused) block page.
 */
static void end_read_io(struct ngnfs_block_info *blinf, struct ngnfs_block *bl,
			struct page *data_page)
{
	if (data_page) {
		/* this means that _block_buf() will change, callers beware */
		if (bl->page)
			put_page(bl->page);
		bl->page = data_page;
		get_page(bl->page);
	}

	if (!test_bit(BL_ERROR, &bl->bits))
		set_bit(BL_UPTODATE, &bl->bits);

	smp_wmb(); /* set error|uptodate before clearing reading */
	clear_bit_and_wake_up(BL_READING, &bl->bits, &bl->waitq);
}

/*
 * Finish write IO on a block in a set.  Once all the blocks are written
 * we clear all the block's association with the set, clear its
 * dirtying, and put it.
 */
static void end_write_io(struct ngnfs_block_info *blinf, struct ngnfs_block *bl)
{
	struct ngnfs_block_set *set = rcu_dereference(bl->set);
	struct ngnfs_block *tmp;

	/* caller called 'cause we weren't reading, should only be dirty writeback */
	BUG_ON(IS_ERR_OR_NULL(set));
	/* XXX not supporting write errors yet (keeping blocks dirty) */
	BUG_ON(test_bit(BL_ERROR, &bl->bits));

	clear_bit(BL_DIRTY, &bl->bits);

	/* each finished block gives room for more writeback in the queue depth */
	atomic_dec(&blinf->nr_writeback);
	try_queue_writeback_work(blinf);

	if (atomic_dec_return(&set->submitted_blocks) > 0)
		return;

	atomic_sub(set->size, &blinf->nr_dirty);
	set->size = 0;

	/*
	 * XXX This many barriers is a bummer, but the block's set
	 * pointer is the serialization point for dirtying.  Once the
	 * pointer is NULL another dirtier can set it and try to use the
	 * set_head.
	 */
	list_for_each_entry_safe(bl, tmp, &set->block_list, set_head) {
		list_del_init(&bl->set_head);
		smp_wmb(); /* empty set_head before clearing set allows redirtying */
		rcu_assign_pointer(bl->set, NULL);
		/* XXX bl refcount? */
	}

	clear_bit_and_wake_up(SET_WRITEBACK, &set->bits, &set->waitq);
	put_set(set);

	/* finishing the whole set could wake sync or dirty waiters */
	if (waitqueue_active(&blinf->waitq))
		wake_up(&blinf->waitq);
}

/*
 * An incoming data_page ref is only used for reads. Writes always
 * manage source page that contains their written contents.
 */
void ngnfs_block_end_io(struct ngnfs_fs_info *nfi, u64 bnr, struct page *data_page, int err)
{
	struct ngnfs_block_info *blinf = nfi->block_info;
	struct ngnfs_block *bl;

	/* XXX describe trying page granular pinning */

	bl = lookup_block(blinf, bnr);
	assert(!IS_ERR_OR_NULL(bl)); /* not supporting this failure yet */

	/* XXX not sure what this means for writeback errors */
	if (err < 0) {
		set_bit(BL_ERROR, &bl->bits);
		bl->error = err;
		sync_waiters_set_error(blinf);
	}

	if (test_bit(BL_READING, &bl->bits))
		end_read_io(blinf, bl, data_page);
	else
		end_write_io(blinf, bl);

	put_block(bl);
}

/*
 * Callers are gathering items that were concurrently appended to a
 * lockless list (llist) and putting them on a private list_head list
 * for private use.  We'd like to preserve list order so we walk the
 * llist lifo and construct a private fifo that is then spliced onto the
 * end of the caller's existing list.
 *
 * This doesn't remove/initialize the llist nodes.  The caller will do
 * that as they iterate over the items the private list.
 */
static void del_all_reverse_add_tail(struct list_head *list, struct llist_head *llist,
				     ssize_t offset)
{
	struct llist_node *node;
	struct llist_node *pos;
	struct list_head *head;
	LIST_HEAD(reverse);

	node = llist_del_all(llist);
	if (node) {
		llist_for_each(pos, node) {
			head = (void *)pos + offset;
			list_add(head, &reverse);
		}
		list_splice_tail(&reverse, list);
	}
}

/*
 * The submit work is responsible for keeping the backend's queue depth
 * full.  This is only concerned with the IO submission pipeline,
 * callers (particularly batch submission preparation) manage higher
 * order concepts like atomic writes.
 */
static void ngnfs_block_submit_work(struct work_struct *work)
{
	struct ngnfs_block_info *blinf = container_of(work, struct ngnfs_block_info, submit_work);
	struct ngnfs_fs_info *nfi = blinf->nfi;
	struct ngnfs_block *tmp;
	struct ngnfs_block *bl;
	int space;
	int ret;
	int op;

	del_all_reverse_add_tail(&blinf->submit_list, &blinf->submit_llist,
				 offsetof(struct ngnfs_block, submit_head) -
				 offsetof(struct ngnfs_block, submit_llnode));
	space = blinf->queue_depth - atomic_read(&blinf->nr_submitted);

	list_for_each_entry_safe(bl, tmp, &blinf->submit_list, submit_head) {
		if (space-- < 0)
			break;

		init_llist_node(&bl->submit_llnode);
		list_del_init(&bl->submit_head);

		/* XXX _GET_WRITE isn't operational yet */
		op = test_bit(BL_READING, &bl->bits) ? NGNFS_BTX_OP_GET_READ : NGNFS_BTX_OP_WRITE;

		atomic_inc(&blinf->nr_submitted);
		ret = blinf->btr_ops->submit_block(nfi, blinf->btr_info, op, bl->bnr, bl->page);
		BUG_ON(ret != 0);

		put_block(bl);
	}
}

/*
 * XXX These empty tests make me nervous.
 */
static void try_queue_submit_work(struct ngnfs_block_info *blinf)
{
	if ((!list_empty(&blinf->submit_list) || !llist_empty(&blinf->submit_llist)) &&
	    (atomic_read(&blinf->nr_submitted) < blinf->queue_depth))
		queue_work(blinf->wq, &blinf->submit_work);
}

/*
 * We submit dirty sets for writeback if either we're syncing sets that
 * haven't been written or sufficient dirty sets have accumulated and
 * there isn't a queue's depth worth of blocks currently in writeback.
 */
static bool should_writeback(struct ngnfs_block_info *blinf)
{
	int dirty = atomic_read(&blinf->nr_dirty);
	int writeback = atomic_read(&blinf->nr_writeback);

	return (atomic64_read(&blinf->sync_seq) > atomic64_read(&blinf->writeback_seq) ||
		((dirty - writeback) >= WRITEBACK_THRESH)) &&
	       (writeback < blinf->queue_depth);
}

static void try_queue_writeback_work(struct ngnfs_block_info *blinf)
{
	if (should_writeback(blinf))
		queue_work(blinf->wq, &blinf->writeback_work);
}

/*
 * The writeback work is responsible for preparing sets for writeback
 * and sending their blocks to the submit work.  Today they're sent
 * directly but eventually we'll want to work with the transports to
 * prepare the blocks.
 */
static void ngnfs_block_writeback_work(struct work_struct *work)
{
	struct ngnfs_block_info *blinf = container_of(work, struct ngnfs_block_info,
						      writeback_work);
	struct ngnfs_block_set *set;
	struct ngnfs_block_set *tmp;
	struct ngnfs_block *bl;

	/* always gather dirtied sets from llist for iteration */
	del_all_reverse_add_tail(&blinf->writeback_list, &blinf->writeback_llist,
				 offsetof(struct ngnfs_block_set, writeback_head) -
				 offsetof(struct ngnfs_block_set, writeback_llnode));

	list_for_each_entry_safe(set, tmp, &blinf->writeback_list, writeback_head) {
		if (!should_writeback(blinf))
			break;

		/* back off if set is dirtying, we'll be queued after */
		BUG_ON(test_bit(SET_WRITEBACK, &set->bits));
		set_bit(SET_WRITEBACK, &set->bits);
		smp_mb(); /* set writeback before testing dirtying */
		if (test_bit(SET_DIRTYING, &set->bits)) {
			clear_bit_and_wake_up(SET_WRITEBACK, &set->bits, &set->waitq);
			wait_event(&set->waitq, !test_bit(SET_DIRTYING, &set->bits));
			break;
		}

		/* list presence ref passes to end_io, get ref to protect block iteration */
		list_del_init(&set->writeback_head);
		if (set->size > 0) {
			atomic_add(set->size, &blinf->nr_writeback);
			atomic_add(set->size, &set->submitted_blocks);
			get_set(set);
			/*
			 * Make sure nr_writeback is visible before
			 * writeback_seq, and that the set is referenced
			 * and has submitted_blocks for end_io via llist
			 * presence.
			 */
			smp_wmb();

			list_for_each_entry(bl, &set->block_list, set_head) {
				get_block(bl);
				llist_add(&bl->submit_llnode, &blinf->submit_llist);
			}
			try_queue_submit_work(blinf);
		}

		atomic64_inc(&blinf->writeback_seq);
		put_set(set);
	}
}

static bool bad_nbf(nbf_t nbf)
{
	return hweight_long(nbf & NBF_RW_EXCL) > 1;
}

/*
 * Acquire a reference to a cached block.  The behaviour of the
 * reference is defined by the block flags as documented at the nbf_t
 * definition.  Successfully acquired references must later be released
 * by calling _put().
 *
 * This doesn't yet differentiate between exclusive read and write
 * references.
 */
struct ngnfs_block *ngnfs_block_get(struct ngnfs_fs_info *nfi, u64 bnr, nbf_t nbf)
{
	struct ngnfs_block_info *blinf = nfi->block_info;
	struct ngnfs_block *bl;
	int err;

	if (WARN_ON_ONCE(bad_nbf(nbf))) {
		bl = ERR_PTR(-EINVAL);
		goto out;
	}

	bl = lookup_or_alloc_block(blinf, bnr);
	if (IS_ERR(bl))
		goto out;

	/* XXX also drop dirty?  hmm. */
	if ((nbf & NBF_NEW)) {
		memset(ngnfs_block_buf(bl), 0, NGNFS_BLOCK_SIZE);
		set_bit(BL_UPTODATE, &bl->bits);
	}

	if (!test_bit(BL_UPTODATE, &bl->bits)) {
		if (!test_and_set_bit(BL_READING, &bl->bits)) {
			get_block(bl); /* presence on submit lists before hitting transport */
			llist_add(&bl->submit_llnode, &blinf->submit_llist);
			try_queue_submit_work(blinf);
		}

		wait_event(&bl->waitq, !test_bit(BL_READING, &bl->bits));
	}

	if (test_bit(BL_ERROR, &bl->bits)) {
		err = bl->error;
		put_block(bl);
		bl = ERR_PTR(err);
	}
out:
	return bl;
}

void ngnfs_block_put(struct ngnfs_block *bl)
{
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
 * Get a reference to a block's set if it's different than the caller's.
 * If the block doesn't have a set then we either add it to the caller's
 * set or allocate a new set for it.  If we add it to the caller's set
 * then we return NULL.  Otherwise we return the block's (possibly newly
 * allocated) set with a reference held, or an err ptr.
 */
static struct ngnfs_block_set *get_other_set(struct ngnfs_block *bl,
					     struct ngnfs_block_set *existing)
{
	struct ngnfs_block_set *set;
	struct ngnfs_block_set *tmp;

	for (;;) {
		/* return other referenced set, or null if block already in existing */
		rcu_read_lock();
		set = rcu_dereference(bl->set);
		if (set && set != existing)
			atomic_inc(&set->refcount);
		rcu_read_unlock();
		if (set) {
			if (set == existing)
				set = NULL;
			break;
		}

		/* return null if we add to caller's existing set */
		if (existing) {
			tmp = unrcu_pointer(cmpxchg(&bl->set, RCU_INITIALIZER(NULL),
						    RCU_INITIALIZER(existing)));
			if (tmp == NULL) {
				list_add_tail(&bl->set_head, &existing->block_list);
				existing->size++;
				break;
			}

			cpu_relax();
			continue;
		}

		/* return newly allocated other set with ref or error */
		set = kmalloc(sizeof(struct ngnfs_block_set), GFP_NOFS);
		if (!set) {
			set = ERR_PTR(-ENOMEM);
			break;
		}

		atomic_set(&set->refcount, 2); /* caller and bl->set pointer */
		atomic_set(&set->submitted_blocks, 0);
		INIT_LIST_HEAD(&set->writeback_head);
		INIT_LIST_HEAD(&set->block_list);
		init_waitqueue_head(&set->waitq);
		set->bits = 0;
		set->size = 1;

		list_add_tail(&bl->set_head, &set->block_list);

		smp_wmb();  /* store initialized fields before setting block set pointer */
		tmp = unrcu_pointer(cmpxchg(&bl->set, RCU_INITIALIZER(NULL),
					    RCU_INITIALIZER(set)));
		if (tmp == NULL)
			break;

		kfree(set);
		cpu_relax();
		continue;
	}

	return set;
}

/*
 * Some of the input blocks built up in the set might not have been
 * dirty.  If we're backing off from dirtying we remove those blocks
 * from the set.  This is done in rare contention cases or when a set
 * would have exceeded the size limit.
 */
static void clear_set_dirtying(struct ngnfs_block_info *blinf, struct ngnfs_block_set *set)
{
	struct ngnfs_block *bl;
	struct ngnfs_block *tmp;

	list_for_each_entry_safe_reverse(bl, tmp, &set->block_list, set_head) {
		if (test_bit(BL_DIRTY, &bl->bits))
			break;
		list_del_init(&bl->set_head);
		smp_wmb(); /* setting set to null is like an unlock */
		rcu_assign_pointer(bl->set, NULL);
		set->size--;
	}

	clear_bit_and_wake_up(SET_DIRTYING, &set->bits, &set->waitq);
	try_queue_writeback_work(blinf);
}

/*
 * _dirty_{begin,end} callers pass in a list of blocks in a weird way.
 * The caller passes in a list of private structs and the offset in each
 * struct to the block pointer for that list element.
 */
#define for_each_dirty_list_block(bl, pos, list, off)					\
	for (pos = list->next;								\
	     (bl = (pos == list ? NULL : *(struct ngnfs_block **)((void *)pos + off)));	\
	     pos = pos->next)

/*
 * The caller has write references to the blocks that it wants to modify
 * together in one transaction.  We walk the blocks and attempt to merge
 * them into one set so that they can be modified and marked dirty.
 *
 * The caller's blocks may be found on multiple existing dirty sets.  As
 * we iterate over the blocks we try to merge each new block's set into
 * a single set.  When merging sets would exceed the set limit we write
 * out the larger of the two sets and try again.
 *
 * Caller's blocks might not yet be dirty.  If we have to write out
 * large sets before merging then we can remove the blocks that were
 * going to be modified but weren't dirty.
 *
 * We're racing with other threads dirtying sets or with the writeback
 * thread writing out sets.  The DIRTYING bit excludes both.
 */
int ngnfs_block_dirty_begin(struct ngnfs_fs_info *nfi, struct list_head *list, ssize_t off)
{
	struct ngnfs_block_info *blinf = nfi->block_info;
	struct ngnfs_block_set *small = NULL;
	struct ngnfs_block_set *large = NULL;
	struct ngnfs_block *bl = NULL;
	struct list_head *pos;
	u64 seq;
	int ret;

	/* maybe some txn pattern can end up harmlessly executing an empty txn */
	if (list_empty(list))
		return 0;

	/* XXX probably interruptible, io errors won't clear dirty */
	wait_event(&blinf->waitq, atomic_read(&blinf->nr_dirty) < DIRTY_LIMIT);

restart:
	put_set(small);
	put_set(large);
	small = NULL;
	large = NULL;
	for_each_dirty_list_block(bl, pos, list, off) {

		/* initially "small" is the set from the next block */
		put_set(small);
		small = get_other_set(bl, large);
		if (IS_ERR(small)) {
			ret = PTR_ERR(small);
			goto out;
		}

		/* block is already in our large set */
		if (small == NULL)
			continue;

		/* wait until set is not being dirtied by someone else */
		if (test_and_set_bit(SET_DIRTYING, &small->bits)) {
			if (large)
				clear_set_dirtying(blinf, large);
			wait_event(&small->waitq, !test_bit(SET_DIRTYING, &small->bits));
			goto restart;
		}

		smp_mb(); /* treat setting dirtying as a lock -- hard load/store barrier */

		/* wait until set is not being written */
		if (test_bit(SET_WRITEBACK, &small->bits)) {
			clear_set_dirtying(blinf, small);
			if (large)
				clear_set_dirtying(blinf, large);
			wait_event(&small->waitq, !test_bit(SET_WRITEBACK, &small->bits));
			goto restart;
		}

		if (!large) {
			/* found first block's set, carry on */
			large = small;
			small = NULL;
			continue;
		}

		/*
		 * Once we have both sets marked DIRTYING we correct the
		 * small/large relationship.  We'll merge small's blocks
		 * into large, and we'll wait for large to be written if
		 * the merge exceeds the set size limit.
		 */
		if (small->size > large->size)
			swap(small, large);

		/* wait for writeback of large if merged size exceeds limit */
		if (large->size + small->size > SET_LIMIT) {
			seq = large->dirty_seq;
			smp_mb(); /* finish with fields under DIRTYING before clearing */
			clear_set_dirtying(blinf, small);
			clear_set_dirtying(blinf, large);

			/* XXX do we want txns to fail with io errors? */
			ret = sync_up_to_seq(blinf, seq);
			if (ret < 0)
				goto out;
			goto restart;
		}

		/* finally merge the smaller set into the larger */
		list_for_each_entry(bl, &small->block_list, set_head)
			rcu_assign_pointer(bl->set, large);
		list_splice_init(&small->block_list, &large->block_list);
		large->size += small->size;
		small->size = 0;
		clear_bit_and_wake_up(SET_DIRTY, &small->bits, &small->waitq);
		clear_bit_and_wake_up(SET_DIRTYING, &small->bits, &small->waitq);
		/* emptied small set will be freed once ref is put */
	}

	/* dirtying and modifying will succeed from this point */

	/* make sure any newly added blocks are dirty */
	list_for_each_entry_reverse(bl, &large->block_list, set_head) {
		if (test_bit(BL_DIRTY, &bl->bits))
			break;
		set_bit(BL_DIRTY, &bl->bits);
		atomic_inc(&blinf->nr_dirty);
	}

	/* initially mark set as dirty and establish its writeback position */
	if (!test_and_set_bit(SET_DIRTY, &large->bits)) {
		/* ref for writeback list presence (and probably through to end_io) */
		get_set(large);
		large->dirty_seq = atomic64_inc_return(&blinf->dirty_seq);
		smp_wmb(); /* store ref get before allowing put via llist presence */
		llist_add(&large->writeback_llnode, &blinf->writeback_llist);
		try_queue_writeback_work(blinf);
	}

	/* pass our large ref on to _dirty_end to clear SET_DIRTYING and put */
	large = NULL;
	ret = 0;
out:
	put_set(small);
	put_set(large);

	return ret;
}

/*
 * The writer is done modifying all the blocks.  _dirty_begin put all
 * the blocks in one set so we just need to get the set from the first
 * block and clear dirtying.
 */
void ngnfs_block_dirty_end(struct ngnfs_fs_info *nfi, struct list_head *list, ssize_t off)
{
	struct ngnfs_block_info *blinf = nfi->block_info;
	struct ngnfs_block_set *set;
	struct ngnfs_block *bl;
	struct list_head *pos;

	for_each_dirty_list_block(bl, pos, list, off) {
		set = rcu_dereference(bl->set);
		clear_bit_and_wake_up(SET_DIRTYING, &set->bits, &set->waitq);
		put_set(set); /* from _dirty_begin */
		break;
	}

	/* XXX hmm, it'd be nice to not always store here (test_and_set when queuing) */
	try_queue_writeback_work(blinf);
}

/*
 * Attempt to write all blocks that were dirty at the time of the call,
 * returning errors from any write failures of those blocks.
 */
int ngnfs_block_sync(struct ngnfs_fs_info *nfi)
{
	struct ngnfs_block_info *blinf = nfi->block_info;

	return sync_up_to_seq(blinf, atomic64_read(&blinf->dirty_seq));
}

int ngnfs_block_setup(struct ngnfs_fs_info *nfi, struct ngnfs_block_transport_ops *btr_ops,
		      void *btr_setup_arg)
{
	struct ngnfs_block_info *blinf;
	int ret;

	blinf = kzalloc(sizeof(struct ngnfs_block_info), GFP_KERNEL);
	if (!blinf)
		return -ENOMEM;

	atomic_set(&blinf->nr_dirty, 0);
	atomic_set(&blinf->nr_writeback, 0);
	atomic_set(&blinf->nr_submitted, 0);
	atomic_set(&blinf->sync_waiters, 0);
	atomic64_set(&blinf->dirty_seq, 0);
	atomic64_set(&blinf->writeback_seq, 0);
	atomic64_set(&blinf->sync_seq, 0);
	init_llist_head(&blinf->submit_llist);
	INIT_LIST_HEAD(&blinf->submit_list);
	init_llist_head(&blinf->writeback_llist);
	INIT_LIST_HEAD(&blinf->writeback_list);
	blinf->nfi = nfi;
	blinf->btr_ops = btr_ops;
	INIT_WORK(&blinf->submit_work, ngnfs_block_submit_work);
	INIT_WORK(&blinf->writeback_work, ngnfs_block_writeback_work);
	init_waitqueue_head(&blinf->waitq);

	if (blinf->btr_ops->setup) {
		blinf->btr_info = blinf->btr_ops->setup(nfi, btr_setup_arg);
		if (IS_ERR(blinf->btr_info)) {
			ret = PTR_ERR(blinf->btr_info);
			goto out;
		}
	}

	blinf->queue_depth = blinf->btr_ops->queue_depth(nfi, blinf->btr_info);

	ret = rhashtable_init(&blinf->ht, &ngnfs_block_ht_params);
	if (ret < 0) {
		kfree(blinf);
		goto out;
	}

	/* XXX use fs identifier in name */
	blinf->wq = create_singlethread_workqueue("ngnfs-workq");
	if (!blinf->wq) {
		rhashtable_destroy(&blinf->ht);
		kfree(blinf);
		ret = -ENOMEM;
		goto out;
	}

	nfi->block_info = blinf;
	ret = 0;
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

	/* XXX make sure this makes sense */
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
		destroy_workqueue(blinf->wq);

		if (blinf->btr_ops->destroy)
			blinf->btr_ops->destroy(nfi, blinf->btr_info);
		rhashtable_free_and_destroy(&blinf->ht, free_ht_block, blinf);
		kfree(blinf);
		nfi->block_info = NULL;
	}
}
