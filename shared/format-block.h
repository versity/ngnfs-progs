/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_FORMAT_BLOCK_H
#define NGNFS_SHARED_FORMAT_BLOCK_H

#include "shared/lk/compiler_attributes.h"
#include "shared/lk/limits.h"
#include "shared/lk/types.h"

#define NGNFS_BLOCK_SHIFT	12
#define NGNFS_BLOCK_SIZE	(1 << NGNFS_BLOCK_SHIFT)

struct ngnfs_btree_ref {
	__le64 bnr;
	/* XXX block alloc counter, too?  hmm. */
};

/*
 * We don't use the maximum size of either the key or value sizes.  It'd
 * be tempting to pack them into fewer bytes but the savings just isn't
 * worth it.  The smallest items are into the tens of bytes so saving a
 * byte doesn't justify the implementation and usability complexity.
 */
struct ngnfs_btree_item {
	__le16 val_size;
	__u8 key_size;
} __packed;

#define NGNFS_BTREE_KEY_SIZE_MAX	U8_MAX
/*
 * We want to avoid there only being a few items in a full block so we
 * chose a reasonably small fraction of the block size.
 */
#define NGNFS_BTREE_VAL_SIZE_MAX	512

struct ngnfs_btree_block {
	__le64 bnr;
	__le16 nr_items;
	__le16 total_free;
	__le16 avail_free;
	__u8 level;
	__u8 _pad;
	__le16 item_off[];
};

/*
 * The minimum utilization of a block, as measured by the percentage of
 * the block after the header that contains items.  As a block's
 * utilization reaches this value it will be refilled from a sibling,
 * merging the two if they're both at the threshold.
 *
 * We set this lower than half the block so that alternating item
 * insertion and deletion doesn't repeatedly split and merge blocks as
 * they straddle a shared threshold.
 */
#define NGNFS_BTREE_MIN_USED_PCT	35

#define NGNFS_BTREE_MAX_FREE	(NGNFS_BLOCK_SIZE - sizeof(struct ngnfs_btree_block))
#define NGNFS_BTREE_MAX_ITEMS								\
	(NGNFS_BTREE_MAX_FREE / (sizeof_field(struct ngnfs_btree_block, item_off[0]) +	\
				 sizeof(struct ngnfs_btree_item) + 1 + 0))

/*
 * The inode block is a btree block with the most significant byte of
 * the key indicating the type of data stored in the item.
 */
#define NGNFS_IBLOCK_KEY_INODE	0

/*
 * Inodes are stored in inode blocks.  Inode blocks numbers are directly
 * calculated from the inode number.  The block itself is formatted as a
 * btree block and the inodes (and other inline inode data) are stored
 * as btree items in the block.
 */
struct ngnfs_inode {
	__le64 ino;
	__le64 gen;
	__le64 size;
	__le64 version;
	__le32 nlink;
	__le32 uid;
	__le32 gid;
	__le32 mode;
	__le32 rdev;
	__le32 flags;
	__le64 atime_nsec;
	__le64 ctime_nsec;
	__le64 mtime_nsec;
	__le64 crtime_nsec;
};

#define NGNFS_ROOT_INO 1

#endif
