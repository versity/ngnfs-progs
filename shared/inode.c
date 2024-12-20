/* SPDX-License-Identifier: GPL-2.0 */

#include "shared/lk/bug.h"
#include "shared/lk/byteorder.h"
#include "shared/lk/errno.h"
#include "shared/lk/kernel.h"
#include "shared/lk/minmax.h"
#include "shared/lk/types.h"

#include "shared/block.h"
#include "shared/format-block.h"
#include "shared/inode.h"
#include "shared/txn.h"

/*
 * For now inode structs are simply stored at the front of blocks.  The
 * block number is the inode number, avoiding false sharing of block
 * access for consecutive inodes.  We're almost certainly going to want
 * to pack inode data into the rest of the block.
 */

int ngnfs_inode_init(struct ngnfs_inode_txn_ref *itref, u64 ino, u64 gen, u32 nlink, umode_t mode,
		     u64 nsec)
{
	struct ngnfs_txn_block *tblk = itref->tblk;
	struct ngnfs_inode *ninode = itref->ninode;

	ngnfs_tblk_assign(tblk, ninode->ino, cpu_to_le64(ino));
	ngnfs_tblk_assign(tblk, ninode->gen, cpu_to_le64(gen));
	ngnfs_tblk_assign(tblk, ninode->size, 0);
	ngnfs_tblk_assign(tblk, ninode->version, cpu_to_le64(1));
	ngnfs_tblk_assign(tblk, ninode->nlink, cpu_to_le32(nlink));
	ngnfs_tblk_assign(tblk, ninode->uid, 0);
	ngnfs_tblk_assign(tblk, ninode->gid, 0);
	ngnfs_tblk_assign(tblk, ninode->mode, cpu_to_le32(mode));
	ngnfs_tblk_assign(tblk, ninode->rdev, 0);
	ngnfs_tblk_assign(tblk, ninode->flags, 0);
	ngnfs_tblk_assign(tblk, ninode->atime_nsec, cpu_to_le64(nsec));
	ngnfs_tblk_assign(tblk, ninode->ctime_nsec, ninode->atime_nsec);
	ngnfs_tblk_assign(tblk, ninode->mtime_nsec, ninode->atime_nsec);
	ngnfs_tblk_assign(tblk, ninode->crtime_nsec, ninode->atime_nsec);
	ngnfs_tblk_memset(tblk, &ninode->dirents, 0, sizeof(struct ngnfs_btree_root));

	return 0;
}

/*
 * XXX should be validating that the block is a valid inode, etc.
 */
int ngnfs_inode_get(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn, nbf_t nbf, u64 ino,
		    struct ngnfs_inode_txn_ref *itref)
{
	return ngnfs_txn_get_block(nfi, txn, ino, nbf, &itref->tblk, (void **)&itref->ninode);
}

int ngnfs_inode_alloc(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn, u64 *ino,
		      struct ngnfs_inode_txn_ref *itref)
{
	u64 bnr;
	int ret;

	ret = ngnfs_txn_alloc_meta(txn, &bnr) ?:
	      ngnfs_inode_get(nfi, txn, NBF_WRITE | NBF_NEW, bnr, itref);
	if (ret == 0)
		*ino = bnr;
	return ret;
}

/*
 * A transaction that just copies the inode in the block to the caller's
 * buffer.
 */
int ngnfs_inode_read_copy(struct ngnfs_fs_info *nfi, u64 ino, void *buf, int size)
{
	struct ngnfs_inode_txn_ref itref;
	struct ngnfs_transaction txn;
	int ret;

	if (WARN_ON_ONCE(size < 0)) {
		ret = -EINVAL;
		goto out;
	}

	ngnfs_txn_init(&txn);

	do {
		ret = ngnfs_inode_get(nfi, &txn, NBF_READ, ino, &itref);
		if (ret == 0) {
			ret = min(size, sizeof(struct ngnfs_inode));
			memcpy(buf, itref.ninode, ret);
		}
	} while (ngnfs_txn_retry(nfi, &txn, &ret));

	ngnfs_txn_teardown(nfi, &txn);
out:
	return ret;
}
