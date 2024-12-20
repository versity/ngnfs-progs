/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_TXN_H
#define NGNFS_SHARED_TXN_H

struct ngnfs_transaction;

#include "shared/lk/byteorder.h"
#include "shared/lk/list.h"
#include "shared/lk/rbtree.h"

#include "shared/block.h"
#include "shared/unbuf.h"

struct ngnfs_txn_block;

#define ngnfs_tblk_assign(tblk, lhs, rhs)			\
do {								\
	__typeof__(lhs) *lhs_ = &(lhs);				\
								\
	ngnfs_txn_save(tblk, lhs_, sizeof(*lhs_));		\
	*lhs_ = (rhs);						\
} while (0)							\

#define ngnfs_tblk_memcpy(tblk, dst, src, n)			\
do {								\
	__typeof__(dst) dst_ = (dst);				\
	__typeof__(n) n_ = (n);					\
								\
	ngnfs_txn_save(tblk, dst_, n_);				\
	memcpy(dst_, (src), n_);				\
} while (0)

#define ngnfs_tblk_memset(tblk, s, c, n)			\
do {								\
	__typeof__(s) s_ = (s);					\
	__typeof__(n) n_ = (n);					\
								\
	ngnfs_txn_save(tblk, s_, n_);				\
	memset(s_, (c), n_);					\
} while (0)

#define ngnfs_tblk_memmove(tblk, dst, src, n)			\
do {								\
	__typeof__(dst) dst_ = (dst);				\
	__typeof__(n) n_ = (n);					\
								\
	ngnfs_txn_save(tblk, dst_, n_);				\
	memmove(dst_, (src), n_);				\
} while (0)

#define ngnfs_tblk_le16_add_cpu(tblk, lep, val)			\
do {								\
	__typeof__(lep) lep_ = (lep);				\
								\
	ngnfs_txn_save(tblk, lep_, sizeof(*lep_));		\
	le16_add_cpu(lep_, (val));				\
} while (0)

/*
 * Given a (buf) buffer of (total) size, zero the tail bytes	\
 * after (head) initial bytes.					\
 * skipping (head) initial bytes.
 */
#define ngnfs_tblk_zero_tail(tblk, buf, head, total)		\
do {								\
	__typeof__(head) head_ = (head);			\
	__typeof__(head) tail_ = (total) - head_;		\
								\
	if (tail_ > 0)						\
		ngnfs_tblk_memset((tblk), (void *)(buf) + head_, 0, tail_); \
} while (0)

/*
 * We expose the type so callers can allocate and initialize it, but they don't
 * use it directly.
 */
struct ngnfs_transaction {
	struct rb_root blocks;
	struct list_head writes;
	bool retry_triggered;
};

#define INIT_NGNFS_TXN(txn) {				\
	.blocks = RB_ROOT,				\
	.writes = LIST_HEAD_INIT(txn.writes),		\
	.retry_triggered = false,			\
}

static inline void ngnfs_txn_init(struct ngnfs_transaction *txn)
{
	txn->blocks = RB_ROOT;
	INIT_LIST_HEAD(&txn->writes);
	txn->retry_triggered = false;
}

int ngnfs_txn_get_block(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn,
			u64 bnr, nbf_t nbf, struct ngnfs_txn_block **tblk_ret, void **data_ret);
int ngnfs_txn_alloc_meta(struct ngnfs_transaction *txn, u64 *bnr_ret);
void ngnfs_txn_save(struct ngnfs_txn_block *tblk, void *ptr, size_t size);
bool ngnfs_txn_retry(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn, int *ret);
void ngnfs_txn_teardown(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn);

#endif
