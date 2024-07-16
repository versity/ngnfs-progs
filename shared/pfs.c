/* SPDX-License-Identifier: GPL-2.0 */

/*
 * POSIX FS operations are implemented in terms of block transaction
 * callbacks.
 *
 * Each operation describes the blocks that need to be read or written
 * atomically to perform the operation.
 *
 * The caller provides the txn object that the operation is built in.
 * This lets the caller hold the locks built up by the txn while they
 * consume and act on the result of the operation before tearing down
 * the txn which drops locks.
 *
 * This could also let us support multiple operations in one
 * transaction, though the api would need to be expanded a bit.
 */

#include "shared/lk/bug.h"
#include "shared/lk/byteorder.h"
#include "shared/lk/errno.h"
#include "shared/lk/types.h"

#include "shared/block.h"
#include "shared/btree.h"
#include "shared/format-block.h"
#include "shared/pfs.h"
#include "shared/txn.h"

/*
 * The metadata for a given inode is stored in a full block with a
 * direct mapping from the inode number.  XXX this would want to detect
 * bad inode numbers?
 */
static inline int map_iblock(u64 *bnr, u64 ino)
{
	*bnr = ino;
	return 0;
}

static void commit_mkfs(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn,
			struct ngnfs_block *bl, void *arg)
{
	struct ngnfs_inode *ninode = arg;
	u8 key = NGNFS_IBLOCK_KEY_INODE;
	struct ngnfs_btree_block *bt;
	int ret;

	bt = ngnfs_block_buf(bl);
	ngnfs_btree_init_block(bt, 0);

	ret = ngnfs_btree_insert(bt, &key, sizeof(key), ninode, sizeof(struct ngnfs_inode));
	BUG_ON(ret != 0);
}

/*
 * As ever, mkfs is a bit of a special case because it's building up the
 * structures that we'd otherwise be using to make metadata changes.
 */
int ngnfs_pfs_mkfs(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn,
		   u64 root_ino, u64 nsec)
{
	struct ngnfs_inode *ninode;
	u64 bnr;
	int ret;

	/* keeping the ninode copy off the stack */
	ninode = kmalloc(sizeof(struct ngnfs_inode), GFP_NOFS);
	if (!ninode) {
		ret = -ENOMEM;
		goto out;
	}

	ninode->ino = cpu_to_le64(root_ino);
	ninode->gen = cpu_to_le64(1);
	ninode->nlink = cpu_to_le32(1); /* "." */
	ninode->mode = cpu_to_le32(0755);
	ninode->atime_nsec = cpu_to_le64(nsec);
	ninode->ctime_nsec = ninode->atime_nsec;
	ninode->mtime_nsec = ninode->atime_nsec;
	ninode->crtime_nsec = ninode->atime_nsec;

	ret = map_iblock(&bnr, root_ino) ?:
	      ngnfs_txn_add_block(nfi, txn, bnr, NBF_WRITE, NULL, commit_mkfs, ninode) ?:
	      ngnfs_txn_execute(nfi, txn);
	kfree(ninode);
out:
	return ret;
}

struct read_inode_args {
	struct ngnfs_inode *ninode;
	size_t size;
	int ret;
};
static int prepare_read_inode(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn,
			      struct ngnfs_block *bl, void *arg)
{
	struct ngnfs_btree_block *bt = ngnfs_block_buf(bl);
	struct read_inode_args *args = arg;
	u8 key = NGNFS_IBLOCK_KEY_INODE;

	args->ret = ngnfs_btree_lookup(bt, &key, sizeof(key), args->ninode, args->size);
	return 0;
}

/*
 * Copy the inode struct from its inode block item into the caller's buffer, returning
 * the size copied.
 */
int ngnfs_pfs_read_inode(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn, u64 ino,
			 struct ngnfs_inode *ninode, size_t size)
{
	struct read_inode_args args = {
		.ninode = ninode,
		.size = size,
	};
	u64 bnr;
	int ret;

	ret = map_iblock(&bnr, ino) ?:
	      ngnfs_txn_add_block(nfi, txn, bnr, NBF_READ, prepare_read_inode, NULL, &args) ?:
	      ngnfs_txn_execute(nfi, txn);
	ngnfs_txn_destroy(nfi, txn);

	return ret ?: args.ret;
}
