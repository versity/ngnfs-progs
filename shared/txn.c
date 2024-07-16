/* SPDX-License-Identifier: GPL-2.0 */

#include "shared/lk/err.h"
#include "shared/lk/errno.h"
#include "shared/lk/list.h"

#include "shared/block.h"
#include "shared/txn.h"

/*
 * Callers describe multi-block transactions as a set of blocks with
 * access modes.  As we try to acquire access to each block we call
 * ->prepare so that it can access the block contents and perhaps add
 * more blocks.  Once all blocks are prepared we call ->commit on all
 * the blocks.
 *
 * This frees the caller from needing to worry about the access
 * acquisition rules.  They assemble the blocks as they see fit and we
 * ensure that we safely acquire access to them without deadlocking.
 *
 * The commit functions make changes to blocks once prepare has ensured
 * that all the changes will succeed.  This avoids unwinding in the face
 * of error.  We work with the block cache to ensure that the blocks are
 * written as an atomic unit as well.
 */

struct ngnfs_transaction_block {
	struct list_head head;
	struct list_head write_head;
	struct ngnfs_block *bl;
	u64 bnr;
	nbf_t nbf;
	txn_prepare_fn prepare;
	txn_commit_fn commit;
	void *arg;
};

/* off = bl - head -> bl = head + off */
#define WRITE_HEAD_BL_OFFSET \
	(ssize_t)(offsetof(struct ngnfs_transaction_block, bl) - \
		  offsetof(struct ngnfs_transaction_block, write_head))

void ngnfs_txn_init(struct ngnfs_transaction *txn)
{
	INIT_LIST_HEAD(&txn->blocks);
	INIT_LIST_HEAD(&txn->writes);
}

/*
 * In theory it's legitimate to implement a "transaction" by just
 * acquiring a mix of read and write access on blocks without doing
 * anything, so we don't enforce that prepare or commit are non-null.
 */
int ngnfs_txn_add_block(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn, u64 bnr,
			nbf_t nbf, txn_prepare_fn prepare, txn_commit_fn commit, void *arg)
{
	struct ngnfs_transaction_block *tblk;
	int ret;

	tblk = kmalloc(sizeof(struct ngnfs_transaction_block), GFP_NOFS);
	if (!tblk) {
		ret = -ENOMEM;
		goto out;
	}

	INIT_LIST_HEAD(&tblk->write_head);
	tblk->bl = NULL;
	tblk->bnr = bnr;
	tblk->nbf = nbf;
	tblk->prepare = prepare;
	tblk->commit = commit;
	tblk->arg = arg;

	list_add_tail(&tblk->head, &txn->blocks);
	ret = 0;
out:
	return ret;
}

/*
 * Callers are responsible for tearing down the txn.
 */
int ngnfs_txn_execute(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn)
{
	struct ngnfs_transaction_block *tblk;
	struct ngnfs_block *bl;
	int ret = 0;

	list_for_each_entry(tblk, &txn->blocks, head) {
		bl = ngnfs_block_get(nfi, tblk->bnr, tblk->nbf);
		if (IS_ERR(bl)) {
			ret = PTR_ERR(bl);
			goto out;
		}

		tblk->bl = bl;

		if (tblk->prepare) {
			ret = tblk->prepare(nfi, txn, tblk->bl, tblk->arg);
			if (ret < 0)
				goto out;
		}

		if (tblk->nbf & NBF_WRITE)
			list_add_tail(&tblk->write_head, &txn->writes);
	}

	if (!list_empty(&txn->writes)) {
		ret = ngnfs_block_dirty_begin(nfi, &txn->writes, WRITE_HEAD_BL_OFFSET);
		if (ret < 0)
			goto out;

		list_for_each_entry(tblk, &txn->writes, write_head) {
			if (tblk->commit)
				tblk->commit(nfi, txn, tblk->bl, tblk->arg);
		}

		ngnfs_block_dirty_end(nfi, &txn->writes, WRITE_HEAD_BL_OFFSET);
	}

out:
	return ret;
}

/*
 * Tear down a transaction.  The transaction must have been initialized
 * and this can be called for any state of the transaction, including
 * repeatedly.  It is a nop on a newly initialized or previously
 * destroyed txn.  The caller is responsible for the allocation of the
 * txn struct itself.
 */
void ngnfs_txn_destroy(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn)
{
	struct ngnfs_transaction_block *tblk;
	struct ngnfs_transaction_block *tmp;

	list_for_each_entry_safe(tblk, tmp, &txn->blocks, head) {
		if (!list_empty(&tblk->write_head))
			list_del_init(&tblk->write_head);
		list_del_init(&tblk->head);
		ngnfs_block_put(tblk->bl);
		kfree(tblk);
	}
}
