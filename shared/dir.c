/* SPDX-License-Identifier: GPL-2.0 */

#include "shared/lk/align.h"
#include "shared/lk/bitops.h"
#include "shared/lk/build_bug.h"
#include "shared/lk/bug.h"
#include "shared/lk/byteorder.h"
#include "shared/lk/container_of.h"
#include "shared/lk/errno.h"
#include "shared/lk/kernel.h"
#include "shared/lk/ktime.h"
#include "shared/lk/limits.h"
#include "shared/lk/math.h"
#include "shared/lk/minmax.h"
#include "shared/lk/stat.h"
#include "shared/lk/stddef.h"
#include "shared/lk/string.h"
#include "shared/lk/types.h"
#include "shared/lk/xxhash.h"

#include "shared/block.h"
#include "shared/btree.h"
#include "shared/dir.h"
#include "shared/format-block.h"
#include "shared/inode.h"
#include "shared/txn.h"

/*
 * Directory entries are stored as items in btrees rooted in the
 * directory's inode.  The key of the btree item is a hash of the name,
 * and the item value contains the entry's metadata.
 *
 * Readdir uses the key values as the readdir position, so we ensure
 * that there's one entry per key.  We manually assign low bits of the
 * key to resolve hash collisions, returning enospc if collisions use
 * all the bits.
 */

static bool names_equal(u8 *a, size_t a_len, u8 *b, size_t b_len)
{
	return a_len == b_len && memcmp(a, b, a_len) == 0;
}

static u64 name_hash(void *name, size_t name_len)
{
	return xxh64(name, name_len, NGNFS_DIRENT_HASH_SEED) & NGNFS_DIRENT_HASH_MASK;
}

/*
 * These large dirent structs can fit a full sized name so that we can
 * copy in and out any dirent as we work with them.
 */
struct dirent_args {
	u64 hash;
	size_t dent_size;
	mode_t mode;
	u64 ino;

	struct ngnfs_dirent dent;
	u8 __max_name_storage[NGNFS_NAME_MAX - sizeof_field(struct ngnfs_dirent, name)];
};

static void init_dirent_key(struct ngnfs_btree_key *key, u64 hash)
{
	*key = (struct ngnfs_btree_key) {
		.k[0] = cpu_to_le64(hash),
	};
}

/* XXX :) */
static u8 ngnfs_dt(umode_t mode)
{
	return 0;
}

static void init_dirent_args(struct dirent_args *da, char *name, size_t name_len, u64 ino,
			     mode_t mode)
{
	da->hash = name_hash(name, name_len);
	da->dent_size = sizeof(struct ngnfs_dirent) + name_len;
	da->mode = mode;

	da->dent.ino = cpu_to_le64(ino);
	da->dent.version = cpu_to_le64(0); /* XXX :/ */
	da->dent.dtype = ngnfs_dt(mode);
	da->dent.name_len = name_len;

	/* ensure that we're stitching together a contiguous max name buffer */
	BUILD_BUG_ON(offsetofend(struct dirent_args, dent.name) !=
		     offsetof(struct dirent_args, __max_name_storage));
	BUILD_BUG_ON((sizeof_field(struct dirent_args, dent.name) +
		      sizeof_field(struct dirent_args, __max_name_storage)) != NGNFS_NAME_MAX);

	memcpy(da->dent.name, name, name_len);
}

static int update_dirent_args_ino(struct dirent_args *da, u64 ino)
{
	da->dent.ino = cpu_to_le64(ino);

	return 0;
}

static inline int check_ifmt(struct ngnfs_inode *ninode, u32 ifmt, int err)
{
	/* XXX mixing persistent structures and stat abi constants? */

	return ((le32_to_cpu(ninode->mode) & S_IFMT) == ifmt) ? 0 : err;
}

/*
 * Update a directory's inode to reflect creation.  We can return errors
 * if the create should fail.
 */
static int update_dir(struct ngnfs_txn_block *tblk, struct ngnfs_inode *dir,
		      struct dirent_args *da, int posneg)
{
	s32 delta;
	int ret;

	if (S_ISDIR(da->mode)) {
		delta = posneg * 1;
		if ((le32_to_cpu(dir->nlink) + delta >= NGNFS_LINK_MAX)) {
			ret = -EMLINK;
			goto out;
		}
		ngnfs_tblk_assign(tblk, dir->nlink, cpu_to_le32(le32_to_cpu(dir->nlink) + delta));
	}

	/* dir i_size includes null termed names */
	delta = posneg * ((s32)da->dent.name_len + 1);
	ngnfs_tblk_assign(tblk, dir->size, cpu_to_le64(le64_to_cpu(dir->size) + delta));
	ret = 0;
out:
	return ret;
}

/*
 * Insert a new dirent by iterating over the existing dirent items which
 * collide with the caller's hashed name value.  If we see a matching
 * name we return eexist.  We insert into the first free hash value we
 * see, returning enospc if they're all taken.
 *
 * This stops iterating once it inserts at a free value, which only
 * works if there's two possible values.  This could be refactored to
 * support multiple collision values, but it introduces a subtle error
 * case if we want to continue only having one iteration for insertion.
 * If we first insert at a low missing collision value then we can mask
 * eexist from later colliding names with insertion errors.
 *
 * We're choosing to only have one collision bit, meaning the third
 * collision returns spurious enospc, to keep this simple and avoid that
 * weird potential error case.
 */
static int insert_dirent_wr(struct ngnfs_btree_key *key, void *val, size_t size, void *arg,
			    struct ngnfs_btree_op *op)
{
	struct ngnfs_dirent *dent = val;
	struct dirent_args *da = arg;

	if (dent) {
		if (names_equal(dent->name, dent->name_len, da->dent.name, da->dent.name_len))
			return -EEXIST;

		if (da->hash == le64_to_cpu(key->k[0])) {
			if (da->hash & NGNFS_DIRENT_COLL_BIT)
				return -ENOSPC;
			da->hash |= NGNFS_DIRENT_COLL_BIT;
			return NGNFS_BTREE_ITER_CONTINUE;
		}
	}

	op->insert = 1;
	init_dirent_key(&op->key, da->hash);
	op->val = &da->dent;
	op->val_size = da->dent_size;

	return 0;
}

/*
 * Insert a dirent item into a directory inode's dirent btree.  The
 * iterator scans colliding items and finds a free collision value for
 * insertion, or returns errors.  The iterator updates the hash value in
 * its dirent_args as it iterates past existing collision bits.
 */
static int insert_dirent(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn,
			 struct ngnfs_inode_txn_ref *dir, struct dirent_args *da)
{
	struct ngnfs_btree_key key;
	struct ngnfs_btree_key last;

	init_dirent_key(&key, da->hash);
	init_dirent_key(&last, da->hash | NGNFS_DIRENT_COLL_BIT);

	return ngnfs_btree_write_iter(nfi, txn, dir->tblk, &dir->ninode->dirents, &key,
				      &last, insert_dirent_wr, da);
}


/*
 * Allocate a new inode and add a directory entry referencing it.
 */
int ngnfs_dir_create(struct ngnfs_fs_info *nfi, u64 dir_ino, umode_t mode, char *name,
		     size_t name_len)
{
	struct {
		struct ngnfs_transaction txn;
		struct ngnfs_inode_txn_ref dir;
		struct ngnfs_inode_txn_ref inode;
		u64 ino;
		u64 nsec;
		struct dirent_args da;
	} *op;
	int ret;

	op = kmalloc(sizeof(*op), GFP_NOFS);
	if (!op) {
		ret = -ENOMEM;
		goto out;
	}

	ngnfs_txn_init(&op->txn);
	op->nsec = ktime_to_ns(ktime_get_real());
	init_dirent_args(&op->da, name, name_len, 0, mode);

	do {
		ret = ngnfs_inode_get(nfi, &op->txn, NBF_WRITE, dir_ino, &op->dir)		?:
		      check_ifmt(op->dir.ninode, S_IFDIR, -ENOTDIR)				?:
		      ngnfs_inode_alloc(nfi, &op->txn, &op->ino, &op->inode)			?:
		      ngnfs_inode_init(&op->inode, op->ino, 1, 1, mode, op->nsec)		?:
		      update_dirent_args_ino(&op->da, op->ino)					?:
		      insert_dirent(nfi, &op->txn, &op->dir, &op->da)				?:
		      update_dir(op->dir.tblk, op->dir.ninode, &op->da, 1);

	} while (ngnfs_txn_retry(nfi, &op->txn, &ret));

	ngnfs_txn_teardown(nfi, &op->txn);
	kfree(op);
out:
	return ret;
}

struct readdir_args {
	struct ngnfs_readdir_entry *ent;
	size_t size;
	int nr;
};

static int fill_dirent_rd(struct ngnfs_btree_key *key, void *val, size_t val_size, void *args)
{
	struct readdir_args *ra = args;
	struct ngnfs_dirent *dent = val;
	size_t aligned;
	size_t bytes;

	bytes = offsetof(struct ngnfs_readdir_entry, name[dent->name_len + 1]);
	if (bytes > ra->size)
		return 0;

	aligned = ALIGN(bytes, __alignof__(struct ngnfs_readdir_entry));

	ra->ent->pos = le64_to_cpu(key->k[0]);
	ra->ent->ino = le64_to_cpu(dent->ino);
	ra->ent->gen = le64_to_cpu(dent->version);
	ra->ent->next_offset = aligned;
	ra->ent->dtype = 0;
	ra->ent->name_len = dent->name_len;
	memcpy(ra->ent->name, dent->name, dent->name_len);

	ra->nr++;

	if (ra->nr == INT_MAX || aligned >= ra->size)
		return 0;

	ra->ent = (void *)ra->ent + aligned;
	ra->size -= aligned;

	return NGNFS_BTREE_ITER_CONTINUE;
}

/*
 * Read directory entries starting at the given position, filling
 * entries in the buffer until entries are exhausted or the buffer is
 * full.  Returns the number of entries filled in the buffer.  There can
 * be padding between the end of one entry and the start of the next so
 * ngnfs_readdir_next_dirent(dent) must be used to iterate and it
 * shouldn't be used on the last filled entry.
 *
 * This only uses one call to the btree read iter so we don't have to
 * unwind the entries stored during a transaction that retried.
 *
 * The caller can continue iteration past the last entry returned by
 * incrementing that entrty's pos.
 *
 * (XXX would be nice to have a flag in the entry to say that we reached
 * the last entry so that the caller doesn't have to discover eof with a
 * final call that returns nothing.)
 */
int ngnfs_dir_readdir(struct ngnfs_fs_info *nfi, u64 dir_ino, u64 pos,
		      struct ngnfs_readdir_entry *buf, size_t size)
{
	struct ngnfs_inode_txn_ref dir;
	struct ngnfs_transaction txn;
	struct ngnfs_btree_key key;
	struct readdir_args ra;
	int ret;

	if (size < NGNFS_READDIR_MIN_BUF_SIZE) {
		ret = -ENOBUFS;
		goto out;
	}

	ngnfs_txn_init(&txn);
	init_dirent_key(&key, pos);

	do {
		ra.ent = buf;
		ra.size = size;
		ra.nr = 0;

		ret = ngnfs_inode_get(nfi, &txn, NBF_READ, dir_ino, &dir)			?:
		      check_ifmt(dir.ninode, S_IFDIR, -ENOTDIR)					?:
		      ngnfs_btree_read_iter(nfi, &txn, &dir.ninode->dirents, &key,
					    NULL, NULL, fill_dirent_rd, &ra);

	} while (ngnfs_txn_retry(nfi, &txn, &ret));

	ngnfs_txn_teardown(nfi, &txn);
out:
	return ret ?: ra.nr;
}
