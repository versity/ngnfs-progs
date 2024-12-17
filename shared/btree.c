/* SPDX-License-Identifier: GPL-2.0 */

#include "shared/lk/align.h"
#include "shared/lk/bitops.h"
#include "shared/lk/build_bug.h"
#include "shared/lk/bug.h"
#include "shared/lk/byteorder.h"
#include "shared/lk/container_of.h"
#include "shared/lk/errno.h"
#include "shared/lk/kernel.h"
#include "shared/lk/limits.h"
#include "shared/lk/minmax.h"
#include "shared/lk/types.h"
#include "shared/lk/stddef.h"
#include "shared/lk/string.h"

#include "shared/block.h"
#include "shared/btree.h"
#include "shared/compare.h"
#include "shared/format-block.h"
#include "shared/rbt.h"
#include "shared/txn.h"

/*
 * These block btrees are used to sort items with variable size value
 * payloads and unpredictable key distribution.  In the file system,
 * that means dirents, xattrs, and global indices.
 *
 * We use persistent in-block rbtrees to sort the items by their keys.
 * Critical users, particularly the global index, can have blocks with
 * random key distribution and insertion patterns and no value payload
 * at all.  With that, and the undo buffers, we want to minimize stores
 * as much as possible during insertion.  Their logarithmic scaling of
 * operation cost with block size (by way of item count) sets us up well
 * for supporting large block sizes.
 *
 * We don't track free internal space in the blocks.  An allocation
 * offset advances towards the tail as we allocate.  We can compact
 * items in a block to free internal space before splitting a block to
 * satisfy insertion.
 */

/*
 * If a block's total_free reaches this value then we try to move items
 * from a neighbor to fill it above the threshold.  If the neighbor is
 * also at the threshold then the two blocks are merged.
 *
 * We want to leave some slack between the max size of a merged block
 * (80% full) and a full block so that the repeated insertion and
 * deletion of a few items doesn't bounce a pair of blocks between
 * splitting and merging.
 */
#define NGNFS_BTREE_MERGE_FREE_THRESH	(NGNFS_BTREE_MAX_FREE * 40 / 100)

/*
 * If an insertion could be performed after compacting free space, but
 * total free space is less than this threshold, then we'll split the
 * block instead.  This avoids excessive compaction if insert/delete
 * cycles constantly delete to create fragmented space and then try to
 * insert into it.  The higher we set this value the more items need to
 * be involved in the cycle before each compaction, so the lower its
 * amortized cost.
 */
#define NGNFS_BTREE_SPLIT_FREE_THRESH	(NGNFS_BTREE_MAX_FREE * 10 / 100)

/* 0, ~0 don't need endian swapping */
static struct ngnfs_btree_key min_key = { { 0, 0, 0} };
static struct ngnfs_btree_key max_key = { { (__le64 __force)U64_MAX,
					    (__le64 __force)U64_MAX,
					    (__le64 __force)U64_MAX} };

/*
 * This is here for now because we're storing the block number in the
 * btree block header.  This will change as the network block protocol
 * provides metadata for all blocks.
 */
static void init_ref(struct ngnfs_block_ref *ref, struct ngnfs_btree_block *bt)
{
	ref->bnr = bt->bnr;
}

static void init_block(struct ngnfs_txn_block *tblk, struct ngnfs_btree_block *bt,
		       u64 bnr, u8 level, struct ngnfs_btree_key *first,
		       struct ngnfs_btree_key *last)
{
	ngnfs_rbt_init_root(tblk, &bt->item_root);
	ngnfs_tblk_assign(tblk, bt->first, *first);
	ngnfs_tblk_assign(tblk, bt->last, *last);
	ngnfs_tblk_assign(tblk, bt->bnr, cpu_to_le64(bnr));
	ngnfs_tblk_assign(tblk, bt->nr_items, 0);
	ngnfs_tblk_assign(tblk, bt->tail_free, cpu_to_le16(NGNFS_BTREE_MAX_FREE));
	ngnfs_tblk_assign(tblk, bt->total_free, bt->tail_free);
	ngnfs_tblk_memset(tblk, &bt->__pad[0], 0, sizeof(bt->__pad));
	ngnfs_tblk_assign(tblk, bt->level, level);
	ngnfs_tblk_zero_tail(tblk, bt, sizeof(struct ngnfs_btree_block), NGNFS_BLOCK_SIZE);
}

static void bug_on_bad_item_off(size_t off)
{
	BUG_ON(off < sizeof(struct ngnfs_btree_block));
	BUG_ON(off > (NGNFS_BLOCK_SIZE - sizeof(struct ngnfs_btree_item)));
	BUG_ON(!IS_ALIGNED(off, NGNFS_BTREE_ITEM_ALIGN));
}

static struct ngnfs_btree_item *item_from_off(struct ngnfs_btree_block *bt, u16 off)
{
	if (off == 0)
		return NULL;

	bug_on_bad_item_off(off);

	return (void *)bt + off;
}

static struct ngnfs_btree_item *item_from_node_off(struct ngnfs_btree_block *bt, __le16 node_off)
{
	if (node_off == 0)
		return NULL;

	return item_from_off(bt, le16_to_cpu(node_off) - offsetof(struct ngnfs_btree_item, node));
}

static u16 off_from_item(struct ngnfs_btree_block *bt, struct ngnfs_btree_item *item)
{
	size_t off = (void *)item - (void *)bt;

	bug_on_bad_item_off(off);

	return off;
}

static u16 aligned_item_size(u16 val_size)
{
	return ALIGN(sizeof(struct ngnfs_btree_item) + val_size, NGNFS_BTREE_ITEM_ALIGN);
}

static struct ngnfs_btree_item *item_from_node(struct ngnfs_rbt_node *node)
{
	return node ? container_of(node, struct ngnfs_btree_item, node) : NULL;
}

static struct ngnfs_btree_item *first_item(struct ngnfs_btree_block *bt)
{
	return item_from_node(ngnfs_rbt_first(&bt->item_root));
}

static struct ngnfs_btree_item *last_item(struct ngnfs_btree_block *bt)
{
	return item_from_node(ngnfs_rbt_last(&bt->item_root));
}

static struct ngnfs_btree_item *next_item(struct ngnfs_btree_block *bt,
					  struct ngnfs_btree_item *item)
{
	return item_from_node(ngnfs_rbt_next(&bt->item_root, &item->node));
}

static struct ngnfs_btree_item *prev_item(struct ngnfs_btree_block *bt,
					  struct ngnfs_btree_item *item)
{
	return item_from_node(ngnfs_rbt_prev(&bt->item_root, &item->node));
}

static struct ngnfs_btree_item *first_postorder_item(struct ngnfs_btree_block *bt)
{
	return item_from_node(ngnfs_rbt_first_postorder(&bt->item_root));
}

static struct ngnfs_btree_item *next_postorder_item(struct ngnfs_btree_block *bt,
						    struct ngnfs_btree_item *item)
{
	return item_from_node(ngnfs_rbt_next_postorder(&bt->item_root, &item->node));
}

static struct ngnfs_btree_key *last_key(struct ngnfs_btree_block *bt)
{
	struct ngnfs_btree_item *item = last_item(bt);

	return item ? &item->key : NULL;
}

static int compare_keys(struct ngnfs_btree_key *a, struct ngnfs_btree_key *b)
{
	return ngnfs_compare(le64_to_cpu(a->k[0]), le64_to_cpu(b->k[0])) ?:
	       ngnfs_compare(le64_to_cpu(a->k[1]), le64_to_cpu(b->k[1])) ?:
	       ngnfs_compare(le64_to_cpu(a->k[2]), le64_to_cpu(b->k[2]));
}

/*
 * Return for the next item in the tree >= the search key.  This stops
 * searching if it finds an item matching the key.
 */
static struct ngnfs_btree_item *search_next_item(struct ngnfs_btree_block *bt,
						 struct ngnfs_btree_key *key)
{
	struct ngnfs_btree_item *item;
	ngnfs_rbt_dir_t dir;
	int cmp;

	item = item_from_node_off(bt, bt->item_root.node);
	while (item) {
		cmp = compare_keys(key, &item->key);
		if (cmp == 0)
			return item;

		if (cmp < 0)
			dir = NGNFS_RBT_LEFT;
		else
			dir = NGNFS_RBT_RIGHT;

		item = item_from_node_off(bt, item->node.child[dir]);
	}

	return NULL;
}

/*
 * Return for the next item in the tree >= the search key.  This always
 * provides the item before the item that's returned, particularly when
 * null is returned.
 */
static struct ngnfs_btree_item *search_next_prev_item(struct ngnfs_btree_block *bt,
						      struct ngnfs_btree_key *key,
						      struct ngnfs_btree_item **prev)
{
	struct ngnfs_btree_item *item;
	ngnfs_rbt_dir_t dir;
	int cmp;

	*prev = NULL;

	item = item_from_node_off(bt, bt->item_root.node);
	while (item) {
		cmp = compare_keys(key, &item->key);
		if (cmp == 0) {
			*prev = prev_item(bt, item);
			return item;
		}

		if (cmp < 0) {
			dir = NGNFS_RBT_LEFT;
		} else {
			*prev = item;
			dir = NGNFS_RBT_RIGHT;
		}

		item = item_from_node_off(bt, item->node.child[dir]);
	}

	return NULL;
}

/*
 * True if there's room in the block for an insertion, just not
 * contiguously available at the tail of the block.  The caller is using
 * this after having checked if they should split so we don't have to
 * check if the block is too full to justify compacting.
 */
static bool should_compact(struct ngnfs_btree_block *bt, u16 val_size)
{
	u16 size = aligned_item_size(val_size);

	return (size > le16_to_cpu(bt->tail_free)) && (size <= le16_to_cpu(bt->total_free));
}

/*
 * True if the caller should split the block before trying to insert an
 * item with the given val size.
 *
 * We split if the item doesn't fit in free space at all.
 *
 * But we'll also split if the item doesn't fit in tail free space and
 * would fit in fragmented free space, but free space is so low that
 * we're likely to split anyway soon after compaction.
 */
static bool should_split(struct ngnfs_btree_block *bt, u16 val_size)
{
	u16 size = aligned_item_size(val_size);
	u16 total_free = le16_to_cpu(bt->total_free);

	return (size > total_free) ||
	       (size > le16_to_cpu(bt->tail_free) && total_free < NGNFS_BTREE_SPLIT_FREE_THRESH);
}

/*
 * True if the caller should merge after removing an item with the given
 * value size.  The free space gets large enough that the item
 * population must be small enough and we want to pull items from
 * neighboring blocks to restore balance.
 */
static bool should_merge(struct ngnfs_btree_block *bt, u16 val_size)
{
	return le16_to_cpu(bt->total_free) + aligned_item_size(val_size) >=
		NGNFS_BTREE_MERGE_FREE_THRESH;
}

/*
 * Consume free space at the end of the block to create a new item,
 * initialize it with the caller's arguments, and link it into the tree
 * at the parent's link.
 *
 * Because this references an existing item we will not compact items
 * here.  The caller must have ensured that there was sufficient free
 * space for the item.
 */
static struct ngnfs_btree_item *insert_item(struct ngnfs_txn_block *tblk,
					    struct ngnfs_btree_block *bt,
					    struct ngnfs_btree_key *key, void *val, u16 val_size,
					    struct ngnfs_rbt_node *parent, ngnfs_rbt_dir_t dir)
{
	u16 bytes = aligned_item_size(val_size);
	struct ngnfs_btree_item *item;

	BUG_ON(compare_keys(key, &bt->first) < 0);
	BUG_ON(compare_keys(key, &bt->last) > 0);
	BUG_ON(bytes <= le16_to_cpu(bt->tail_free));

	item = item_from_off(bt, NGNFS_BLOCK_SIZE - le16_to_cpu(bt->tail_free));
	ngnfs_tblk_le16_add_cpu(tblk, &bt->tail_free, -bytes);
	ngnfs_tblk_le16_add_cpu(tblk, &bt->total_free, -bytes);
	ngnfs_tblk_le16_add_cpu(tblk, &bt->nr_items, 1);

	ngnfs_tblk_assign(tblk, item->key, *key);
	ngnfs_tblk_assign(tblk, item->val_size, cpu_to_le16(val_size));
	if (val_size) {
		ngnfs_tblk_memcpy(tblk, &item->val[0], val, val_size);
		ngnfs_tblk_zero_tail(tblk, &item->val[0], val_size,
				     ALIGN(val_size, NGNFS_BTREE_ITEM_ALIGN));
	}

	ngnfs_rbt_insert(tblk, &bt->item_root, parent, dir, &item->node);

	return item;
}

/*
 * Given two adjacent nodes in the tree, we can always find the parent
 * node to insert an item between them.
 */
static struct ngnfs_btree_item *insert_item_between(struct ngnfs_txn_block *tblk,
						    struct ngnfs_btree_block *bt,
						    struct ngnfs_btree_key *key,
						    void *val, u16 val_size,
						    struct ngnfs_btree_item *prev,
						    struct ngnfs_btree_item *next)
{
	struct ngnfs_rbt_node *parent;
	ngnfs_rbt_dir_t dir;

	BUG_ON(prev && next_item(bt, prev) != next);
	BUG_ON(next && prev_item(bt, next) != prev);
	BUG_ON(prev && compare_keys(key, &prev->key) <= 0);
	BUG_ON(next && compare_keys(key, &next->key) >= 0);

	if (!prev && !next) {
		parent = NULL;
		dir = NGNFS_RBT_RIGHT;
	} else if (prev && !prev->node.child[NGNFS_RBT_RIGHT]) {
		parent = &prev->node;
		dir = NGNFS_RBT_RIGHT;
	} else {
		parent = &next->node;
		dir = NGNFS_RBT_LEFT;
	}

	return insert_item(tblk, bt, key, val, val_size, parent, dir);
}

/*
 * Delete an item by removing it from the rbt and zeroing its bytes.
 * This almost certainly leaves behind fragmented free space in the
 * block that will later be reclaimed by compaction.
 */
static void delete_item(struct ngnfs_txn_block *tblk, struct ngnfs_btree_block *bt,
			struct ngnfs_btree_item *item)
{
	u16 bytes = aligned_item_size(le16_to_cpu(item->val_size));
	u16 off = off_from_item(bt, item);

	if (off == NGNFS_BLOCK_SIZE - le16_to_cpu(bt->tail_free) - bytes)
		ngnfs_tblk_le16_add_cpu(tblk, &bt->tail_free, bytes);
	ngnfs_tblk_le16_add_cpu(tblk, &bt->total_free, bytes);
	ngnfs_tblk_le16_add_cpu(tblk, &bt->nr_items, -1);

	ngnfs_rbt_delete(tblk, &bt->item_root, &item->node);
	ngnfs_tblk_memset(tblk, item, 0, bytes);
}

static int compact_cmp_by(struct ngnfs_btree_block *by_off,
			  struct ngnfs_btree_item *a, struct ngnfs_btree_item *b)
{
	return by_off ? ngnfs_compare(off_from_item(by_off, a), off_from_item(by_off, b)) :
			compare_keys(&a->key, &b->key);
}

static void compact_insert_by(struct ngnfs_txn_block *tblk, struct ngnfs_btree_block *bt,
			      struct ngnfs_btree_item *item, bool by_key)
{
	struct ngnfs_btree_block *by_off = by_key ? NULL : bt;
	struct ngnfs_btree_item *parent;
	ngnfs_rbt_dir_t dir;
	int cmp;

	/* insert at leaf */
	parent = item_from_node_off(bt, bt->item_root.node);
	dir = NGNFS_RBT_RIGHT;
	while (parent) {
		cmp = compact_cmp_by(by_off, item, parent);
		dir = cmp < 0 ? NGNFS_RBT_LEFT : NGNFS_RBT_RIGHT;
		parent = item_from_node_off(bt, parent->node.child[dir]);
	}

	ngnfs_rbt_insert(tblk, &bt->item_root, parent ? &parent->node : NULL, dir, &item->node);
}

static void compact_sort_by(struct ngnfs_txn_block *tblk, struct ngnfs_btree_block *bt, bool by_key)
{
	struct ngnfs_btree_item *item;

	item = first_postorder_item(bt);
	ngnfs_rbt_init_root(tblk, &bt->item_root);
	for (; item; item = next_postorder_item(bt, item))
		compact_insert_by(tblk, bt, item, by_key);
}

/*
 * Defragment internal free space by moving all the items towards the
 * front of the block, gathering all free space to the end.  Items can
 * be of any sizes so we sort the item rbt by offset, iterate and move
 * in offset order, then sort by key.
 *
 * Doing it this way is expensive at run time but avoids the code and
 * structural complexity of tracking internal free space or also
 * indexing by offset.  We'll see if we can keep compaction frequency
 * low enough to justify the tradeoff.
 */
static void compact_items(struct ngnfs_txn_block *tblk, struct ngnfs_btree_block *bt)
{
	struct ngnfs_btree_item *item;
	struct ngnfs_btree_item *old;
	u16 bytes;
	u16 off;

	if (bt->nr_items == 0 || bt->tail_free == bt->total_free)
		return;

	compact_sort_by(tblk, bt, false);

	off = sizeof(struct ngnfs_btree_block);
	for (item = first_item(bt); item; item = next_item(bt, item)) {
		bytes = aligned_item_size(le16_to_cpu(item->val_size));
		if (off_from_item(bt, item) != off) {
			old = item;
			item = item_from_off(bt, off);
			ngnfs_tblk_memmove(tblk, item, old, bytes);
			ngnfs_rbt_moved(tblk, &bt->item_root, &item->node, &old->node);
		}
		off += bytes;
	}

	compact_sort_by(tblk, bt, true);

	/* zero newly free region before existing free tail free */
	bytes = le16_to_cpu(bt->total_free) - le16_to_cpu(bt->tail_free);
	ngnfs_tblk_memset(tblk, item_from_off(bt, off), 0, bytes);

	ngnfs_tblk_assign(tblk, bt->tail_free, bt->total_free);
}

/*
 * Move items from the source block to the destination block.  
 *
 * We need to compact the destination before we move so that there's
 * room for the moving items.  This is used by splitting and merging
 * which also has to ensure that both of its output blocks are
 * sufficiently compacted to receive an insertion.  We simplify and
 * always compact the source after moving items, as well.
 *
 * Since we're always moving to the end of the dst block we can always
 * insert onto the last item moved in the dst block, at the link in the
 * opposite direction of the move.
 *
 * @to_right moves items in descending order from the end of the src
 * block to the front of the dst block.  When false it moves in the
 * opposite direction: in ascending order from the start of the src
 * block to the end of the dst block.
 *
 * @until_balanced always tries to move at least one item and stops when
 * the dst block has at least as many bytes used by items as the src
 * block.  Otherwise it tries to move all the items from the src block.
 */
static void move_items(struct ngnfs_txn_block *dst_tblk, struct ngnfs_btree_block *dst,
		       struct ngnfs_txn_block *src_tblk, struct ngnfs_btree_block *src,
		       bool to_right, bool until_balanced)
{
	struct ngnfs_rbt_node *parent;
	struct ngnfs_btree_item *from;
	struct ngnfs_btree_item *del;
	struct ngnfs_btree_item *to;
	ngnfs_rbt_dir_t dir;

	BUG_ON(dst == src);

	if (src->nr_items == 0)
		return;

	compact_items(dst_tblk, dst);

	from = to_right ? last_item(src) : first_item(src);
	to = to_right ? first_item(dst) : last_item(dst);
	if (to) {
		parent = &to->node;
		dir = to_right ? NGNFS_RBT_LEFT : NGNFS_RBT_RIGHT;
	} else {
		parent = NULL;
		dir = NGNFS_RBT_RIGHT;
	}

	while (from) {
		to = insert_item(dst_tblk, dst, &from->key, from->val,
				 le16_to_cpu(from->val_size), parent, dir);
		parent = &to->node;

		del = from;
		from = to_right ? prev_item(src, from) : next_item(src, from);
		delete_item(src_tblk, src, del);

		if (until_balanced && le16_to_cpu(dst->total_free) <= le16_to_cpu(src->total_free))
			break;
	}

	compact_items(src_tblk, src);
}

/*
 * Copy the reference from the parent item's value.
 */
static void copy_ref(struct ngnfs_block_ref *ref, struct ngnfs_btree_item *item)
{
	/* XXX should have been caught by verification */
	BUG_ON(le16_to_cpu(item->val_size) != sizeof(struct ngnfs_block_ref));

	memcpy(ref, item->val, sizeof(struct ngnfs_block_ref));
}
/*
 * The caller must have initialized the child's last key for the parent's ref item.
 */
static void insert_parent_ref(struct ngnfs_txn_block *tblk, struct ngnfs_btree_block *parent,
			      struct ngnfs_btree_block *child)
{
	struct ngnfs_btree_item *item;
	struct ngnfs_btree_item *prev;
	struct ngnfs_block_ref ref;

	item = search_next_prev_item(parent, &child->last, &prev);
	BUG_ON(item && compare_keys(&item->key, &child->last) == 0);
	init_ref(&ref, child);
	insert_item_between(tblk, parent, &child->last, &ref, sizeof(ref), prev, item);
}

/*
 * Tracks block references as we traverse the btree.  Splitting and
 * merging updates this to allocate or free parents or to redirect
 * traversal into a newly allocated result of a split.
 */
struct traversal_blocks {
	struct ngnfs_txn_block *parent_tblk;
	struct ngnfs_btree_block *parent;
	struct ngnfs_txn_block *tblk;
	struct ngnfs_btree_block *bt;
};

static void init_traversal_blocks(struct traversal_blocks *trav)
{
	memset(trav, 0, sizeof(struct traversal_blocks));
}

/*
 * The ordered blocks have had items moved between them.  Reset their
 * inner last,first key range boundary to reflect the key of the last
 * item in the left block.
 */
static void reset_key_range_boundary(struct ngnfs_txn_block *left_tblk,
				     struct ngnfs_btree_block *left,
				     struct ngnfs_txn_block *right_tblk,
				     struct ngnfs_btree_block *right)
{
	struct ngnfs_btree_key key;

	BUG_ON(compare_keys(&left->first, &right->last) >= 0);

	key = *last_key(left);
	ngnfs_tblk_assign(left_tblk, left->last, key);
	ngnfs_btree_key_inc(&key);
	ngnfs_tblk_assign(right_tblk, right->first, key);
}

/*
 * Allocate a new btree block and point the root block ref at it.  The
 * caller will initialize it as a new leaf block or new parent block.
 */
static int alloc_root_block(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn,
			    struct ngnfs_txn_block *root_tblk, struct ngnfs_btree_root *root,
			    struct ngnfs_txn_block **tblkp, struct ngnfs_btree_block **btp)
{
	struct ngnfs_block_ref ref;
	u64 bnr;
	int ret;

	ret = ngnfs_txn_alloc_meta(txn, &bnr);
	if (ret < 0)
		goto out;

	ret = ngnfs_txn_get_block(nfi, txn, bnr, NBF_WRITE | NBF_NEW, tblkp, (void **)btp);
	if (ret < 0)
		goto out;

	init_block(*tblkp, *btp, bnr, root->height, &min_key, &max_key);

	init_ref(&ref, *btp);
	ngnfs_tblk_assign(root_tblk, root->ref, ref);
	ngnfs_tblk_assign(root_tblk, root->height, root->height + 1);
	ret = 0;
out:
	return ret;
}

/*
 * See if we can free the root block.  We can either free a parent with
 * a single ref item or a leaf with no items, never both.
 */
static int check_free_root_block(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn,
				 struct ngnfs_txn_block *root_tblk, struct ngnfs_btree_root *root,
				 struct traversal_blocks *trav)
{
	struct ngnfs_btree_block *bt = NULL;
	struct ngnfs_block_ref ref;
	u8 level;

	if (trav->parent && root->ref.bnr == trav->parent->bnr) {
		bt = trav->parent;
		if (le16_to_cpu(bt->nr_items) == 1)
			memcpy(&ref, last_item(bt)->val, sizeof(ref));
		else
			bt = NULL;

	} else if (trav->bt && root->ref.bnr == trav->bt->bnr) {
		bt = trav->bt;
		if (bt->nr_items == 0)
			memset(&ref, 0, sizeof(ref));
		else
			bt = NULL;
	}

	if (bt) {
		level = bt->level;
		/* ret = ngnfs_txn_free_meta(nfi, txn, tblk, le64_to_cpu(bt->bnr)) */
		ngnfs_tblk_assign(root_tblk, root->ref, ref);
		ngnfs_tblk_assign(root_tblk, root->height, level);

		if (bt == trav->parent) {
			trav->parent_tblk = NULL;
			trav->parent = NULL;
		} else {
			trav->tblk = NULL;
			trav->bt = NULL;
		}
	}

	return 0;
}

/*
 * Split a block, moving items to a newly allocated block.  We move
 * items to balance the space they take up, not the number of items.
 * The new block is always empty so we can always move items.  We move
 * items to a new empty block to the left so that we only have to insert
 * a new parent item and don't have to modify the existing parent item's
 * key.
 */
static int split_block(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn,
		       struct ngnfs_txn_block *root_tblk, struct ngnfs_btree_root *root,
		       struct traversal_blocks *trav, struct ngnfs_btree_key *key)
{
	struct ngnfs_txn_block *nei_tblk;
	struct ngnfs_btree_block *nei;
	u64 bnr;
	int ret;

	/* allocate new parent if we don't have one */
	if (!trav->parent) {
		ret = alloc_root_block(nfi, txn, root_tblk, root,
				       &trav->parent_tblk, &trav->parent);
		if (ret < 0)
			goto out;

		init_block(trav->parent_tblk, trav->parent, bnr, trav->bt->level + 1,
			   &min_key, &max_key);
		insert_parent_ref(trav->parent_tblk, trav->parent, trav->bt);
	}

	/* allocate and initialize new nei */
	ret = ngnfs_txn_alloc_meta(txn, &bnr);
	if (ret < 0)
		goto out;

	ret = ngnfs_txn_get_block(nfi, txn, bnr, NBF_WRITE | NBF_NEW, &nei_tblk, (void **)&nei);
	if (ret < 0)
		goto out;

	init_block(nei_tblk, nei, bnr, trav->bt->level, &trav->bt->first, &trav->bt->last);
	move_items(nei_tblk, nei, trav->tblk, trav->bt, false, true);
	reset_key_range_boundary(nei_tblk, nei, trav->tblk, trav->bt);
	insert_parent_ref(trav->parent_tblk, trav->parent, nei);

	/*
	 * Continue the caller's traversal through the split nei if
	 * we moved the key to the nei.
	 */
	if (compare_keys(key, &nei->last) <= 0) {
		trav->tblk = nei_tblk;
		trav->bt = nei;
	}

	ret = 0;
out:
	return ret;
}

/*
 * Merge items from a neighboring block into our block.  This is only
 * called if there is a parent block so there must be at least one
 * neighbor.
 *
 * Our block can be on either spine of the tree so we need to be able to
 * pull from a neighbor on either side.  We have to update the key in
 * the parent reference item that separates the items in the two child
 * blocks, regardless.
 */
static int merge_block(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn,
		       struct ngnfs_txn_block *root_tblk, struct ngnfs_btree_root *root,
		       struct traversal_blocks *trav)
{
	struct ngnfs_btree_item *nei_ref_item;
	struct ngnfs_btree_item *ref_item;
	struct ngnfs_txn_block *nei_tblk;
	struct ngnfs_btree_block *nei;
	struct ngnfs_block_ref ref;
	bool until_balanced;
	bool to_right;
	u64 bnr;
	int ret;

	/* find our and neighboring ref items */
	ref_item = search_next_item(trav->parent, &trav->bt->last);
	BUG_ON(!ref_item);

	nei_ref_item = next_item(trav->parent, ref_item);
	if (nei_ref_item) {
		to_right = false;
	} else {
		nei_ref_item = prev_item(trav->parent, ref_item);
		BUG_ON(!nei_ref_item); /* must have nei when we have parent */
		to_right = true;
	}

	/* get neighboring block */
	copy_ref(&ref, nei_ref_item);
	bnr = le64_to_cpu(ref.bnr);
	ret = ngnfs_txn_get_block(nfi, txn, bnr, NBF_WRITE, &nei_tblk, (void **)&nei);
	if (ret < 0)
		goto out;

	/* balance items between blocks if together they're both above threshold */
	until_balanced = (le16_to_cpu(trav->bt->total_free) + le16_to_cpu(nei->total_free)) >
			 (NGNFS_BTREE_MERGE_FREE_THRESH * 2);

	/* expand our range so we can insert nei's items without triggering assertions */
	if (to_right)
		ngnfs_tblk_assign(trav->tblk, trav->bt->first, nei->first);
	else
		ngnfs_tblk_assign(trav->tblk, trav->bt->last, nei->last);

	move_items(trav->tblk, trav->bt, nei_tblk, nei, to_right, until_balanced);

	/* if nei has items then use separator to update ranges, and update its parent ref */
	if (nei->nr_items != 0) {
		if (to_right) {
			reset_key_range_boundary(nei_tblk, nei, trav->tblk, trav->bt);
			ngnfs_tblk_assign(trav->parent_tblk, nei_ref_item->key, nei->last);
		} else {
			reset_key_range_boundary(trav->tblk, trav->bt, nei_tblk, nei);
		}
	}

	/* update our parent ref if our last changed */
	if (!to_right)
		ngnfs_tblk_assign(trav->parent_tblk, ref_item->key, trav->bt->last);

	/* delete ref to empty neighbor, maybe free parent with single item */
	if (nei->nr_items == 0) {
		delete_item(trav->parent_tblk, trav->parent, nei_ref_item);
		ret = check_free_root_block(nfi, txn, root_tblk, root, trav);
		if (ret < 0)
			goto out;
	}

	ret = 0;
out:
	return ret;
}

/*
 * Ensure that the traversal's bt block will maintain the btree
 * invariant after inserting or deleting an item with the given
 * val_size.  We're promising the caller that they will be able to
 * insert or delete if we return success.
 *
 * We split if inserting an item with the given val size doesn't fit in
 * the block.  We merge if deleting an item of the given val_size would
 * bring the utilization of the block under the merge threshold.  We
 * also compact if we did neither and compaction would make room for an
 * insertion.
 *
 * The caller can tell us to return denied if we would have split/merged
 * but they didn't want us to.  This saves the caller from having to
 * test the split/merge conditions before calling.
 *
 * Returns < 0 on error, 0 if nothing was done, and > 0 with the TSM_
 * indications of what happened.  The caller can check for DENIED and
 * then use >0 to determine that the items changed (and they can't trust
 * previously held item pointers).
 */
enum {
	TSM_SPLIT_MERGED = 1,
	TSM_COMPACTED,
	TSM_DENIED,
};
static int try_split_merge(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn,
			   struct ngnfs_txn_block *root_tblk, struct ngnfs_btree_root *root,
			   struct traversal_blocks *trav, struct ngnfs_btree_key *key,
			   size_t val_size, bool deny)
{
	bool splitting;
	bool merging;
	u64 bnr;
	int ret;

	splitting = should_split(trav->bt, val_size);
	merging = !splitting && trav->parent && should_merge(trav->bt, val_size);

	if (splitting || merging) {
		if (deny) {
			ret = TSM_DENIED;
			goto out;
		}
		/* try to convert parent and block access to write, may retry */
		if (trav->parent) {
			bnr = le64_to_cpu(trav->parent->bnr);
			ret = ngnfs_txn_get_block(nfi, txn, bnr, NBF_WRITE, &trav->parent_tblk,
						  (void **)&trav->parent);
			if (ret < 0)
				goto out;
		}
		bnr = le64_to_cpu(trav->bt->bnr);
		ret = ngnfs_txn_get_block(nfi, txn, bnr, NBF_WRITE, &trav->tblk,
					  (void **)&trav->bt);
		if (ret < 0)
			goto out;

		if (splitting)
			ret = split_block(nfi, txn, root_tblk, root, trav, key);
		else
			ret = merge_block(nfi, txn, root_tblk, root, trav);
		if (ret < 0)
			goto out;
		ret = TSM_SPLIT_MERGED;

	} else if (should_compact(trav->bt, val_size)) {
		compact_items(trav->tblk, trav->bt);
		ret = TSM_COMPACTED;

	} else {
		ret = 0;
	}

out:
	return ret;
}

/*
 * This doesn't store block refs in traversable_blocks because the
 * readers don't need parents nor write txn block pointers for
 * modification.
 */
static int readable_leaf(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn,
			 struct ngnfs_btree_root *root, struct ngnfs_btree_block **btp,
			 struct ngnfs_btree_key *key)
{
	struct ngnfs_btree_item *item;
	struct ngnfs_btree_block *bt;
	struct ngnfs_block_ref ref;
	int level;
	u64 bnr;
	int ret;

	ref = root->ref;
	bt = NULL;

	for (level = root->height - 1; level >= 0; level--) {
		bnr = le64_to_cpu(ref.bnr);
		ret = ngnfs_txn_get_block(nfi, txn, bnr, NBF_READ, NULL, (void **)&bt);
		if (ret < 0)
			goto out;

		if (level > 0) {
			item = search_next_item(bt, key);
			if (!item) {
				/* XXX corruption, parents must always have child ref */
				ret = -EIO;
				goto out;
			}

			/* XXX relies on block verification */
			copy_ref(&ref, item);
		}
	}

	ret = 0;
out:
	if (ret < 0)
		bt = NULL;
	*btp = bt;

	return ret;
}

/*
 * Walk the btree to a leaf block that contains the given key, setting
 * the caller's traversal parent and bt to point to the leaf and its
 * parent.  Can return success and have both null pointers when the tree
 * is empty.  We split and merge the parents such that the caller can
 * split or merge the leaf once before needing to walk the tree again to
 * split and merge parents.
 */
static int writable_leaf(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn,
			 struct ngnfs_txn_block *root_tblk, struct ngnfs_btree_root *root,
			 struct traversal_blocks *trav, struct ngnfs_btree_key *key)
{
	struct ngnfs_btree_item *item;
	struct ngnfs_block_ref ref;
	int level;
	nbf_t nbf;
	u64 bnr;
	int ret;

	init_traversal_blocks(trav);

	ref = root->ref;
	for (level = root->height - 1; level >= 0; level--) {
		/* start by getting read access to parents, write to only leaf */
		nbf = level == 0 ? NBF_WRITE : NBF_READ;
		bnr = le64_to_cpu(ref.bnr);
		ret = ngnfs_txn_get_block(nfi, txn, bnr, nbf, &trav->tblk, (void **)&trav->bt);
		if (ret < 0)
			goto out;

		if (level == 0)
			break;

		/* ensure that parent is prepared for child split/merge */
		ret = try_split_merge(nfi, txn, root_tblk, root, trav, key,
				      sizeof(struct ngnfs_block_ref), false);
		if (ret < 0)
			goto out;

		item = search_next_item(trav->bt, key);
		if (!item) {
			/* XXX corruption, must always have child <= key */
			ret = -EIO;
			goto out;
		}

		/* XXX relies on block verification */
		copy_ref(&ref, item);
		trav->parent_tblk = trav->tblk;
		trav->parent = trav->bt;
		trav->tblk = NULL;
		trav->bt = NULL;
	}

	ret = 0;
out:
	if (ret < 0)
		init_traversal_blocks(trav);

	return ret;
}

void ngnfs_btree_key_inc(struct ngnfs_btree_key *key)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(key->k); i++) {
		le64_add_cpu(&key->k[i], 1);
		if (key->k[i])
			break;
	}
}

void ngnfs_btree_key_set_min(struct ngnfs_btree_key *key)
{
	*key = min_key;
}

bool ngnfs_btree_key_is_min(struct ngnfs_btree_key *key)
{
	return compare_keys(key, &min_key) == 0;
}

void ngnfs_btree_key_set_max(struct ngnfs_btree_key *key)
{
	*key = max_key;
}

bool ngnfs_btree_key_is_max(struct ngnfs_btree_key *key)
{
	return compare_keys(key, &max_key) == 0;
}

/*
 * A read only traversal through the items found in a leaf block from
 * the given key.  We hold on to read access of all the parent blocks
 * that we descend through in case we need to retry.
 *
 * If the last key is specified then it will be the last possible item
 * that can be called with the iterator fn.
 *
 * If @next is non-null then it is set on return to the key that can be
 * provided to continue iteration.  If it is the min key then iteration
 * is done.
 *
 * This limits the number of leaf blocks that will be traversed in one
 * call to limit the number of blocks referenced by the caller's
 * transaction.  We somewhat arbitrarily chose the number of blocks.
 * Even just two blocks can use twice the height if they straddle the
 * edge of substrees divided by the first parents.  Additional blocks
 * beyond that don't substantially increase the worst case number of
 * blocks referenced.
 */
int ngnfs_btree_read_iter(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn,
			  struct ngnfs_btree_root *root, struct ngnfs_btree_key *key,
			  struct ngnfs_btree_key *next, struct ngnfs_btree_key *last,
			  ngnfs_btree_read_iter_fn_t iter, void *iter_arg)
{
	struct ngnfs_btree_item *item;
	struct ngnfs_btree_block *bt;
	struct ngnfs_btree_key pos;
	int leaf_limit = 4;
	int ret;

	if (next)
		ngnfs_btree_key_set_min(next);
	pos = *key;

	for (;;) {
		ret = readable_leaf(nfi, txn, root, &bt, &pos);
		if (ret < 0 || bt == NULL) /* null bt is catching empty tree */
			goto out;

		for (item = search_next_item(bt, key); item; item = next_item(bt, item)) {
			if (last && compare_keys(&item->key, last) > 0) {
				ret = 0;
				goto out;
			}

			ret = iter(&item->key, item->val, le16_to_cpu(item->val_size), iter_arg);
			if (ret != NGNFS_BTREE_ITER_CONTINUE)
				goto out;
		}

		/* done if block contained last key */
		if ((last && compare_keys(&bt->last, last) >= 0) ||
		    (!last && ngnfs_btree_key_is_max(&bt->last))) {
			ret = 0;
			goto out;
		}

		/* continue on to next leaf */
		pos = bt->last;
		ngnfs_btree_key_inc(&pos);

		/* but finish if leaf limit reached */
		if (--leaf_limit == 0) {
			if (next)
				*next = pos;
			ret = 0;
			goto out;
		}
	}

out:
	return ret;
}

/*
 * Modify items in the btree at the instruction of the caller's iterator
 * callback.  We call the iterator for every item within the caller's
 * range.
 *
 * The callback can set its op argument on return to tell us to delete
 * the existing iterated item or to insert an item before the iterated
 * item.
 *
 * The iterator will always be called on the final key in the range.  If
 * an item doesn't exist with that key then the item value argument for
 * the callback will be NULL and the size will be 0.
 *
 * Traversal to the leaf only ensures that the parent have enough items
 * or free space to maintain the btree invariant after one split/merge.
 * If iteration needs to split or merge again it will back off and
 * traverse again.
 *
 * XXX today it's up to the caller to know to only call with modifications
 * that will fit in a very small number of leaves.  We'll want to expand this
 * a bit to provide a key for continuing iteration.
 */
int ngnfs_btree_write_iter(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn,
			   struct ngnfs_txn_block *root_tblk, struct ngnfs_btree_root *root,
			   struct ngnfs_btree_key *key, struct ngnfs_btree_key *last,
			   ngnfs_btree_write_iter_fn_t iter, void *iter_arg)
{
	struct ngnfs_btree_item *item;
	struct ngnfs_btree_item *prev;
	struct ngnfs_btree_item *next;
	struct traversal_blocks trav;
	struct ngnfs_btree_key pos;
	struct ngnfs_btree_op op;
	bool deny_split_merge;
	int ret;

	pos = *key;

	for (;;) {
		deny_split_merge = false;
		ret = writable_leaf(nfi, txn, root_tblk, root, &trav, &pos);
		if (ret < 0)
			goto out;


		/*
		 * While iterating we maintain prev/next as items that
		 * surround insertion.  We use the item pointer to
		 * record if we're calling with a null final item at the
		 * caller's last key value to end iteration.  We don't
		 * use the res parent or link.
		 */
		if (trav.bt) {
			next = search_next_prev_item(trav.bt, &pos, &prev);
		} else {
			prev = NULL;
			next = NULL;
		}

		for (;;) {
			/* advance to next leaf when it has items within caller last */
			if (!next && trav.bt && compare_keys(&trav.bt->last, last) < 0) {
				pos = trav.bt->last;
				ngnfs_btree_key_inc(&pos);
				break;
			}

			/* don't call on items past last */
			item = next;
			if (item && compare_keys(&item->key, last) > 0)
				item = NULL;

			memset(&op, 0, sizeof(struct ngnfs_btree_op));
			if (item)
				ret = iter(&item->key, item->val, le16_to_cpu(item->val_size),
					   iter_arg, &op);
			else
				ret = iter(last, NULL, 0, iter_arg, &op);
			if (ret != NGNFS_BTREE_ITER_CONTINUE)
				goto out;

			if (WARN_ON_ONCE(op.delete && !item)) {
				ret = -ENOENT;
				goto out;
			}

			/* alloc new leaf block if tree is empty */
			if (op.insert && !trav.bt) {
				ret = alloc_root_block(nfi, txn, root_tblk, root,
						       &trav.tblk, &trav.bt);
				if (ret < 0)
					goto out;
			}

			if (op.delete) {
				op.key = item->key;
				op.val_size = le16_to_cpu(item->val_size);
			}

			if (op.insert || op.delete) {
				ret = try_split_merge(nfi, txn, root_tblk, root, &trav,
						      &op.key, op.val_size, deny_split_merge);
				if (ret < 0)
					goto out;
				if (ret == TSM_DENIED) {
					/* rewalk to leaf so we can split/merge again */
					if (item)
						pos = item->key;
					else
						pos = trav.bt->last;
					break;
				}
				if (ret > 0) {
					if (ret == TSM_SPLIT_MERGED)
						deny_split_merge = true;
					next = search_next_prev_item(trav.bt, &pos, &prev);
					if (item)
						item = next;
				}
			}

			if (op.insert) {
				prev = insert_item_between(trav.tblk, trav.bt, &op.key, op.val,
							       op.val_size, prev, next);
			} else if (op.delete) {
				next = next_item(trav.bt, item);
				delete_item(trav.tblk, trav.bt, item);
				ret = check_free_root_block(nfi, txn, root_tblk, root, &trav);
				if (ret < 0)
					goto out;
			} else if (item) {
				prev = item;
				next = next_item(trav.bt, prev);
			} else {
				/* done after calling iter with last */
				ret = 0;
				goto out;
			}
		}
	}

	ret = 0;
out:
	return ret;
}
