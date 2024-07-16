/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_TXN_H
#define NGNFS_SHARED_TXN_H

struct ngnfs_transaction;

#include "shared/block.h"
#include "shared/lk/list.h"

/*
 * We expose the type so callers can allocate and initialize it, but they don't
 * use it directly.
 */
struct ngnfs_transaction {
	struct list_head blocks;
	struct list_head writes;
};

#define INIT_NGNFS_TXN(txn) {				\
	.blocks = LIST_HEAD_INIT(txn.blocks),		\
	.writes = LIST_HEAD_INIT(txn.writes),		\
}

typedef int (*txn_prepare_fn)(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn,
			      struct ngnfs_block *bl, void *arg);
/*
 * Commit functions should be quick and can not fail.  Prepare's job is
 * to ensure that commit can proceed or return errors.
 */
typedef void (*txn_commit_fn)(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn,
			      struct ngnfs_block *bl, void *arg);

void ngnfs_txn_init(struct ngnfs_transaction *txn);
int ngnfs_txn_add_block(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn, u64 bnr,
			nbf_t nbf, txn_prepare_fn prepare, txn_commit_fn commit, void *arg);
int ngnfs_txn_execute(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn);
void ngnfs_txn_destroy(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn);

#endif
