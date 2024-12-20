/* SPDX-License-Identifier: GPL-2.0 */

#include "shared/lk/atomic.h"
#include "shared/lk/bug.h"
#include "shared/lk/err.h"
#include "shared/lk/errno.h"
#include "shared/lk/list.h"

#include "shared/block.h"
#include "shared/compare.h"
#include "shared/format-block.h"
#include "shared/txn.h"
#include "shared/unbuf.h"

struct ngnfs_txn_block {
	struct rb_node node;
	struct list_head write_head;
	struct ngnfs_block *bl;
	struct ngnfs_undo_buf *unbuf;
	u64 bnr;
	nbf_t requested_nbf;
	nbf_t acquired_nbf;
	bool deadlocked;
};

struct rb_insert_args {
	struct rb_node *parent;
	struct rb_node **link;
};

static struct ngnfs_txn_block *find_tblk(struct rb_root *root, u64 bnr, struct rb_insert_args *ins)
{
	struct ngnfs_txn_block *tblk;
	int cmp;

	ins->parent = NULL;
	ins->link = &root->rb_node;

	while (*ins->link) {
		ins->parent = *ins->link;
		tblk = container_of(*ins->link, struct ngnfs_txn_block, node);

		cmp = ngnfs_compare(bnr, tblk->bnr);
		if (cmp == 0)
			return tblk;

		if (cmp < 0)
			ins->link = &(*ins->link)->rb_left;
		else
			ins->link = &(*ins->link)->rb_right;
	}

	return NULL;
}

static struct ngnfs_txn_block *containing_tblk(struct rb_node *node)
{
	return node ? container_of(node, struct ngnfs_txn_block, node) : NULL;
}

static struct ngnfs_txn_block *last_tblk(struct rb_root *root)
{
	return containing_tblk(rb_last(root));
}

static struct ngnfs_txn_block *next_tblk(struct ngnfs_txn_block *tblk)
{
	return containing_tblk(tblk ? rb_next(&tblk->node) : NULL);
}

static struct ngnfs_txn_block *prev_tblk(struct ngnfs_txn_block *tblk)
{
	return containing_tblk(tblk ? rb_prev(&tblk->node) : NULL);
}

static void put_block(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn,
		      struct ngnfs_txn_block *tblk, nbf_t nbf)
{
	nbf |= tblk->acquired_nbf;

	if ((nbf & (NBF_READ|NBF_WRITE)) == 0)
		return;

	if (nbf & NBF_WRITE)
		list_del_init(&tblk->write_head);

	ngnfs_block_put(nfi, tblk->bl, nbf);

	tblk->bl = NULL;
	tblk->acquired_nbf = 0;
}

static int get_block(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn,
		     struct ngnfs_txn_block *tblk, nbf_t nbf)
{
	struct ngnfs_block *bl;
	int ret;

	/* we want to retry requested mode if it fails */
	tblk->requested_nbf = nbf & (NBF_READ|NBF_WRITE);

	/* always wait for dirty to fall under the limit before we start writing */
	if ((nbf & NBF_WRITE) && list_empty(&txn->writes))
		ngnfs_block_dirty_limit_wait(nfi);

	bl = ngnfs_block_get(nfi, tblk->bnr, nbf);
	if (IS_ERR(bl)) {
		ret = PTR_ERR(bl);
		if (ret == -EDEADLK)
			tblk->deadlocked = true;
		goto out;
	}

	tblk->bl = bl;
	tblk->acquired_nbf = tblk->requested_nbf;

	if (nbf & NBF_WRITE)
		list_add_tail(&tblk->write_head, &txn->writes);

	if (!tblk->unbuf) {
		ret = ngnfs_unbuf_alloc(ngnfs_block_buf(bl), NGNFS_BLOCK_SIZE, &tblk->unbuf);
		if (ret < 0) {
			put_block(nfi, txn, tblk, 0);
			goto out;
		}
	}

	ret = 0;
out:
	return ret;
}

/*
 * Write references can satisfy both, otherwise reads must match.
 */
static bool nbf_rw_compatible(nbf_t have, nbf_t want)
{
	return (have & NBF_WRITE) || (have & want & NBF_READ);
}

/*
 * Get a shared read or exclusive write reference to blocks as part of a
 * transaction.
 *
 * Callers request blocks in whatever traversal order they happen to
 * implement.  We define ordered acquisition as ascending block number.
 * The transaction notices when a reference is requested out of order
 * and attempts a trylock, returning an indication to retry if blocking
 * could deadlock.
 *
 * While most transactions will reference a reasonably small number of
 * blocks, there are a few transactions have an exceptional amount.  In
 * particular, renames across directories can read thousands of parent
 * dir blocks for particularly pathological dir trees.
 */
int ngnfs_txn_get_block(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn,
			u64 bnr, nbf_t nbf, struct ngnfs_txn_block **tblk_ret, void **data_ret)
{
	struct ngnfs_txn_block *tblk;
	struct rb_insert_args ins;
	void *data;
	int ret;

	/*
	 * Our callers specify very few flags, the rest we generate when calling the cache.
	 */
	if (WARN_ON_ONCE(nbf & ~(NBF_READ|NBF_WRITE|NBF_TRY))) {
		ret = -EINVAL;
		goto out;
	}

	tblk = find_tblk(&txn->blocks, bnr, &ins);
	if (!tblk) {
		tblk = kmalloc(sizeof(struct ngnfs_txn_block), GFP_NOFS);
		if (!tblk) {
			ret = -ENOMEM;
			goto out;
		}

		RB_CLEAR_NODE(&tblk->node);
		INIT_LIST_HEAD(&tblk->write_head);
		tblk->bl = NULL;
		tblk->unbuf = NULL;
		tblk->bnr = bnr;
		tblk->requested_nbf = 0;
		tblk->acquired_nbf = 0;

		rb_link_node(&tblk->node, ins.parent, ins.link);
		rb_insert_color(&tblk->node, &txn->blocks);
	}

	/* done if we already have a compatible reference */
	if (nbf_rw_compatible(tblk->acquired_nbf, nbf)) {
		ret = 0;
		goto out;
	}

	/*
	 * txn callers can request a write reference to a block that
	 * they have a read reference to.  (retry will never do this).
	 * The block cache will do its best to satisfy the request but
	 * might not be able to.  If it fails we'll get a write
	 * reference as we retry.
	 */
	if ((nbf & NBF_WRITE) && (tblk->acquired_nbf & NBF_READ))
		nbf |= NBF_CONVERT_WRITE;

	ret = get_block(nfi, txn, tblk, nbf);
out:
	if (ret < 0) {
		tblk = NULL;
		data = NULL;
	} else {
		data = ngnfs_block_buf(tblk->bl);
	}

	if (tblk_ret)
		*tblk_ret = tblk;
	if (data_ret)
		*data_ret = data;

	return ret;
}

/*
 * Transactions maintain an allocation cursor.  It tracks allocation
 * blocks with available blocks that it has write access to.  Our
 * transaction model allows an allocation to use frees within the
 * allocation so we may want to track blocks that we free into here as
 * well.
 *
 * XXX except that allocation blocks don't exist and we're not doing any
 * of that :).  Each new mount will start allocating from the same
 * global fake block number and will overwrite anything that was written
 * by previous mounts.
 */
static atomic64_t global_fake_alloc = ATOMIC64_INIT(NGNFS_ROOT_INO + 1);

int ngnfs_txn_alloc_meta(struct ngnfs_transaction *txn, u64 *bnr_ret)
{
	*bnr_ret = atomic64_inc_return(&global_fake_alloc);
	return 0;
}

/*
 * A caller with a transaction is about to modify a region of block
 * contents.  Use the block's undo buffer to save away the original
 * contents in case we need to restore it.
 */
void ngnfs_txn_save(struct ngnfs_txn_block *tblk, void *ptr, size_t size)
{
	ngnfs_unbuf_save(tblk->unbuf, ptr, size);
}

static void free_tblk(struct ngnfs_transaction *txn, struct ngnfs_txn_block *tblk, bool erase)
{
	BUG_ON(tblk->acquired_nbf & (NBF_READ|NBF_WRITE));

	if (erase && !RB_EMPTY_NODE(&tblk->node))
		rb_erase(&tblk->node, &txn->blocks);
	if (!list_empty(&tblk->write_head))
		list_del_init(&tblk->write_head);
	ngnfs_unbuf_free(tblk->unbuf);

	kfree(tblk);
}

/*
 * Transaction callers call this as their loop body finishes.  On
 * success (ret >= 0) we do nothing and don't have the loop retry.  On
 * all errors we'll undo the modification of block contents made by the
 * transaction.
 *
 * If we're returning a hard error then we let teardown free the
 * transaction as efficiently as possible.
 *
 * If the error indicates that we should retry then we more carefully
 * release block references to the point of deadlock, then block
 * reacquiring references in order before the caller retries.
 *
 * (XXX we'll want to mark blocks that were not found by retries so we
 * can just drop them.)
 */
bool ngnfs_txn_retry(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn, int *ret)
{
	struct ngnfs_txn_block *tblk;
	bool retry = false;

	if (*ret >= 0)
		goto out;

	/* undo block modifications on error */
	list_for_each_entry(tblk, &txn->writes, write_head)
		ngnfs_unbuf_restore(tblk->unbuf);

	if (*ret != -EDEADLK)
		goto out;

	/* put references to all blocks after the bnr that failed */
	for (tblk = last_tblk(&txn->blocks); tblk; tblk = prev_tblk(tblk)) {

		put_block(nfi, txn, tblk, NBF_NODIRTY);

		if (tblk->deadlocked) {
			tblk->deadlocked = false;
			break;
		}
	}

	/* get blocking references in order from the failed bnr  */
	for (; tblk; tblk = next_tblk(tblk)) {
		*ret = get_block(nfi, txn, tblk, tblk->requested_nbf);
		if (*ret < 0)
			goto out;
	}

	*ret = 0;
	retry = true;
out:
	return retry;
}

/*
 * Free all the resources in the txn so that it is safe for the caller
 * to free the txn struct memory itself, or perhaps reuse it.
 */
void ngnfs_txn_teardown(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn)
{
	struct ngnfs_txn_block *tblk;
	struct ngnfs_txn_block *tmp;

	rbtree_postorder_for_each_entry_safe(tblk, tmp, &txn->blocks, node) {
		put_block(nfi, txn, tblk, 0);
		free_tblk(txn, tblk, false);
	}

	txn->blocks = RB_ROOT;
}
