/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_BTREE_H
#define NGNFS_SHARED_BTREE_H

#include "shared/lk/limits.h"

#include "shared/format-block.h"
#include "shared/txn.h"

/*
 * The intent is for these iteration functions to be able to return
 * -errno, 0, .. INT_MAX to be able to stop iteration and return those
 * values to the caller.  If _ITER_CONTINUE is returned then the return
 * is ignored and iteration continues.  It's a negated impossibly large
 * errno so it shouldn't be confused with valid -errno.
 */
#define NGNFS_BTREE_ITER_CONTINUE	INT_MIN

typedef int (*ngnfs_btree_read_iter_fn_t)(struct ngnfs_btree_key *key, void *val,
					  size_t val_size, void *arg);


/*
 * Setting both insert and delete is allowed, it overwrites the existing
 * items value with the op's value.
 */
struct ngnfs_btree_op {
	struct ngnfs_btree_key key;
	void *val;
	size_t val_size;
	unsigned insert:1,
		 delete:1;
};

/*  val can be NULL when called for the last key in range that doesn't exist */
typedef int (*ngnfs_btree_write_iter_fn_t)(struct ngnfs_btree_key *key, void *val, size_t size,
					   void *arg, struct ngnfs_btree_op *op);

void ngnfs_btree_key_inc(struct ngnfs_btree_key *key);
void ngnfs_btree_key_set_min(struct ngnfs_btree_key *key);
bool ngnfs_btree_key_is_min(struct ngnfs_btree_key *key);
void ngnfs_btree_key_set_max(struct ngnfs_btree_key *key);
bool ngnfs_btree_key_is_max(struct ngnfs_btree_key *key);

int ngnfs_btree_read_iter(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn,
			  struct ngnfs_btree_root *root, struct ngnfs_btree_key *key,
			  struct ngnfs_btree_key *next, struct ngnfs_btree_key *last,
			  ngnfs_btree_read_iter_fn_t iter, void *iter_arg);
int ngnfs_btree_write_iter(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn,
			   struct ngnfs_txn_block *root_tblk, struct ngnfs_btree_root *root,
			   struct ngnfs_btree_key *key, struct ngnfs_btree_key *last,
			   ngnfs_btree_write_iter_fn_t iter, void *iter_arg);

#endif
