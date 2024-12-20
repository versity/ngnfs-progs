/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_INODE_H
#define NGNFS_SHARED_INODE_H

#include "shared/block.h"
#include "shared/fs_info.h"
#include "shared/txn.h"

/*
 * Transactions work with references to inode struct in blocks.
 */
struct ngnfs_inode_txn_ref {
	struct ngnfs_txn_block *tblk;
	struct ngnfs_inode *ninode;
};

int ngnfs_inode_init(struct ngnfs_inode_txn_ref *itref, u64 ino, u64 gen, u32 nlink, umode_t mode,
		     u64 nsec);
int ngnfs_inode_get(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn, nbf_t nbf, u64 ino,
		    struct ngnfs_inode_txn_ref *itref);
int ngnfs_inode_alloc(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn, u64 *ino,
		      struct ngnfs_inode_txn_ref *itref);
int ngnfs_inode_read_copy(struct ngnfs_fs_info *nfi, u64 ino, void *buf, int size);

#endif
