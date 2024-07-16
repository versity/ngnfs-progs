/* SPDX-License-Identifier: GPL-2.0 */

#include "shared/lk/bitops.h"
#include "shared/lk/bug.h"
#include "shared/lk/byteorder.h"
#include "shared/lk/container_of.h"
#include "shared/lk/errno.h"
#include "shared/lk/math.h"
#include "shared/lk/minmax.h"
#include "shared/lk/types.h"
#include "shared/lk/sort.h"
#include "shared/lk/stddef.h"
#include "shared/lk/string.h"
#include "shared/lk/unaligned.h"

#include "shared/btree.h"
#include "shared/format-block.h"

/*
 * The internal block format which stores items is balancing operational
 * cost with structural complexity.  We have users which want different
 * key sizes and either no values or values that could use the entire
 * block if we let them (large xattr values).
 *
 * Inode index keys are a few u64s so aligning keys with any additional
 * metadata adds significant overhead, decreasing the fanout and
 * increasing tree height.  We pack everything at byte offsets and take
 * the hit of performing unaligned access.
 *
 * We verify blocks as we read so we want as few degrees of freedom as
 * possible that would have to be checked.
 *
 * These low level btree core functions are concerned with modifying the
 * structures in blocks.  Callers manage block serialization, reading,
 * writing, allocation, and freeing.
 *
 * Item's keys and values are copied to and from buffers in the callers,
 * avoiding having to worry about referencing block contents after we
 * return.
 *
 * XXX:
 *  - zero freed space as we move/compact/delete items
 *  - per-cpu item sorting offset array to avoid double sort
 */

/*
 * Item space utilization includes its entry in the item_off array.
 */
#define ITEM_OFF_SIZE	sizeof_field(struct ngnfs_btree_block, item_off[0])

/*
 * The key follows the small size header in each item.
 */
static void *key_ptr(struct ngnfs_btree_item *item)
{
	return (void *)(item + 1);
}

/*
 * The item's val is stored after the key.
 */
static void *val_ptr(struct ngnfs_btree_item *item)
{
	return key_ptr(item) + item->key_size;
}

/*
 * [gs]etters let us assert on oob accesses.
 */
static inline u16 get_item_off(struct ngnfs_btree_block *bt, u16 pos)
{
	BUG_ON(pos > le16_to_cpu(bt->nr_items));

	return le16_to_cpu(bt->item_off[pos]);
}

static inline void set_item_off(struct ngnfs_btree_block *bt, u16 pos, u16 off)
{
	BUG_ON(pos > le16_to_cpu(bt->nr_items));

	bt->item_off[pos] = cpu_to_le16(off);
}

static inline struct ngnfs_btree_item *item_ptr(struct ngnfs_btree_block *bt, const u16 pos)
{
	return (void *)bt + get_item_off(bt, pos);
}

static inline struct ngnfs_btree_item *last_item_ptr(struct ngnfs_btree_block *bt)
{
	return item_ptr(bt, le16_to_cpu(bt->nr_items) - 1);
}

/*
 * The size taken up by the item struct payload stored at the offset,
 * does not include the item_off array element.
 */
static inline u16 key_val_size(u16 key_size, u16 val_size)
{
	return sizeof(struct ngnfs_btree_item) + key_size + val_size;
}
static inline u16 item_size(struct ngnfs_btree_item *item)
{
	return key_val_size(item->key_size, le16_to_cpu(item->val_size));
}

/*
 * The total bytes consumed by an item, includes the item_off element
 * and the struct payload.
 */
static inline u16 total_item_size(struct ngnfs_btree_item *item)
{
	return ITEM_OFF_SIZE + item_size(item);
}

/*
 * Return the byte offset after the avail_free region.  An item's size is often then subtracted
 * from this to find its allocated offset.
 */
static inline u16 avail_free_end(struct ngnfs_btree_block *bt)
{
	return offsetof(struct ngnfs_btree_block, item_off[le16_to_cpu(bt->nr_items)]) +
	       le16_to_cpu(bt->avail_free);
}

static inline void set_avail_free_end(struct ngnfs_btree_block *bt, u16 off)
{
	bt->avail_free = cpu_to_le16(off - offsetof(struct ngnfs_btree_block,
						    item_off[le16_to_cpu(bt->nr_items)]));
}

/*
 * The number of bytes used by items, this is 0 in a new block.
 */
static inline u16 used_size(struct ngnfs_btree_block *bt)
{
	return NGNFS_BLOCK_SIZE - (sizeof(struct ngnfs_btree_block) + le16_to_cpu(bt->total_free));
}

static inline u16 used_pct(struct ngnfs_btree_block *bt)
{
	return (u32)used_size(bt) * 100 / NGNFS_BTREE_MAX_FREE;
}

/*
 * Move item offset array elements starting with the given position to
 * the end of the array by the relative distance number of elements.
 * The will modify nr_items after the move.
 */
static inline void memmove_tail_offs(struct ngnfs_btree_block *bt, u16 pos, s16 distance)
{
	s16 nr = le16_to_cpu(bt->nr_items) - pos;

	if (nr > 0 && distance != 0)
		memmove(&bt->item_off[pos + distance], &bt->item_off[pos],
			nr * sizeof(bt->item_off[0]));
}

/*
 * Compare two variable length keys with big-endian key material.
 * Larger keys compare larger than smaller keys that are a prefix of the
 * larger.
 */
static inline int cmp_keys(const void *key_a, const u16 size_a,
			   const void *key_b, const u16 size_b)
{
	return memcmp(key_a, key_b, min(size_a, size_b)) ?: ((int)size_b - (int)size_a);
}

/*
 * Find the position in the item offset array that an item with the
 * given key would occupy.  This can return the next position after the
 * current array if the item would be inserted after all the existing
 * items.
 */
struct btree_search_result {
	u16 pos;
	s16 cmp;
};
static struct btree_search_result btree_search(struct ngnfs_btree_block *bt, void *key,
					       u16 key_size)
{
	struct btree_search_result res = { .pos = 0, .cmp = 1 };
	struct ngnfs_btree_item *item;
	s16 first = 0;
	s16 last = le16_to_cpu(bt->nr_items) - 1;

	while (first <= last) {
		res.pos = (first + last) >> 1;
		item = item_ptr(bt, res.pos);
		res.cmp = cmp_keys(key, key_size, key_ptr(item), item->key_size);

		if (res.cmp == 0)
			break;
		else if (res.cmp < 0)
			last = res.pos - 1;
		else
			first = ++res.pos;
	}

	return res;
}

static void insert_item(struct ngnfs_btree_block *bt, u16 pos, void *key, size_t key_size,
			void *val, size_t val_size)
{
	struct ngnfs_btree_item *item;
	u16 size;
	u16 off;

	BUG_ON(pos > le16_to_cpu(bt->nr_items));

	size = key_val_size(key_size, val_size);

	/* XXX callers are checking that there's room?  how about compaction? */
	BUG_ON(le16_to_cpu(bt->avail_free) < (ITEM_OFF_SIZE + size));

	off = avail_free_end(bt) - size;
	memmove_tail_offs(bt, pos, 1);
	set_item_off(bt, pos, off);

	le16_add_cpu(&bt->nr_items, 1);
	le16_add_cpu(&bt->total_free, -(ITEM_OFF_SIZE + size));
	le16_add_cpu(&bt->avail_free, -(ITEM_OFF_SIZE + size));

	item = item_ptr(bt, pos);
	item->key_size = key_size;
	put_unaligned_le16(val_size, &item->val_size);
	memcpy(key_ptr(item), key, key_size);
	memcpy(val_ptr(item), val, val_size);
}

static void remove_item(struct ngnfs_btree_block *bt, u16 pos)
{
	struct ngnfs_btree_item *item;
	u16 tot;

	BUG_ON(pos > le16_to_cpu(bt->nr_items));

	item = item_ptr(bt, pos);
	tot = total_item_size(item);

	le16_add_cpu(&bt->total_free, tot);

	if (get_item_off(bt, pos) == avail_free_end(bt))
		le16_add_cpu(&bt->avail_free, tot);
	else
		le16_add_cpu(&bt->avail_free, ITEM_OFF_SIZE);

	memmove_tail_offs(bt, pos + 1, -1);
	set_item_off(bt, le16_to_cpu(bt->nr_items) - 1, 0);
	le16_add_cpu(&bt->nr_items, -1);
}

/*
 * Move items from the end of one btree block to the opposite end of
 * another.
 *
 * @src_first tells us if we're moving from the first item in the src to
 * the last item in the dst, or vice versa.
 *
 * @drain_src tells us if we're moving all the items from the src into
 * the dst or if we're trying to balance the space consumed by the items
 * in the blocks.  We always move at least one item, even if we weren't
 * draining and the item utilization of the blocks is equal as we're
 * called.
 */
static void move_items(struct ngnfs_btree_block *dst, struct ngnfs_btree_block *src,
		       bool src_first, bool drain_src)
{
	struct ngnfs_btree_item *src_item;
	struct ngnfs_btree_item *dst_item;
	s16 target;
	s16 moving;
	u16 size;
	u16 off;
	u16 nr;
	u16 s;
	u16 d;
	s16 i;

	BUG_ON(le16_to_cpu(src->nr_items) == 0);

	/* find the number of items to move */
	if (drain_src) {
		nr = le16_to_cpu(src->nr_items);
		moving = used_size(src);
	} else {
		target = (used_size(src) - used_size(dst)) >> 1;
		nr = 0;
		moving = 0;
		if (src_first) {
			for (i = 0; moving <= target && i < le16_to_cpu(src->nr_items); i++, nr++)
				moving += total_item_size(item_ptr(src, i));
		} else {
			for (i = le16_to_cpu(src->nr_items) - 1; i >= 0; i--, nr++)
				moving += total_item_size(item_ptr(src, i));
		}
	}

	/* setup item regions for iterative walk of both regions */
	if (src_first) {
		s = 0;
		d = le16_to_cpu(dst->nr_items) - nr;
	} else {
		s = le16_to_cpu(src->nr_items) - nr;
		d = 0;
	}

	/* compact dst for item alloc, and make room in item off array for incoming items */
	ngnfs_btree_compact(dst);
	if (!src_first)
		memmove_tail_offs(dst, 0, nr);

	off = avail_free_end(dst);
	for (i = 0; i < nr; i++) {
		src_item = item_ptr(src, s + i);
		size = item_size(src_item);

		off -= size;
		set_item_off(dst, d + i, off);
		dst_item = item_ptr(dst, d + i);

		memcpy(dst_item, src_item, size);
	}

	/* collapse the front of the src item offs array after items left */
	if (src_first)
		memmove_tail_offs(src, nr, -nr);

	le16_add_cpu(&src->nr_items, -nr);
	le16_add_cpu(&src->total_free, moving);

	le16_add_cpu(&dst->nr_items, nr);
	le16_add_cpu(&dst->total_free, -moving);
	dst->avail_free = dst->total_free; /* dst was compacted, total == avail */
}

static void init_btree_ref(struct ngnfs_btree_ref *ref, struct ngnfs_btree_block *child)
{
	ref->bnr = child->bnr;
}

/*
 * These updates of the parent items rely on incoming block validation
 * having verified that the items in the parent blocks were of the
 * correct length.
 */
static void insert_parent_item(struct ngnfs_btree_block *bt, u16 pos,
			       struct ngnfs_btree_block *child)
{
	struct ngnfs_btree_item *last = last_item_ptr(child);
	struct ngnfs_btree_ref ref;

	init_btree_ref(&ref, child);

	insert_item(bt, pos, key_ptr(last), last->key_size, &ref, sizeof(ref));
}

static void update_parent_key(struct ngnfs_btree_block *bt, u16 pos,
			      struct ngnfs_btree_block *child)
{
	struct ngnfs_btree_item *item = item_ptr(bt, pos);
	struct ngnfs_btree_item *last = last_item_ptr(child);

	/* should be verified on read */
	BUG_ON(item->key_size != last->key_size);

	memcpy(key_ptr(item), key_ptr(last), last->key_size);
}

static void update_parent_ref(struct ngnfs_btree_block *bt, u16 pos,
			      struct ngnfs_btree_block *child)
{
	struct ngnfs_btree_item *item = item_ptr(bt, pos);
	struct ngnfs_btree_item *last = last_item_ptr(child);
	struct ngnfs_btree_ref ref;

	init_btree_ref(&ref, child);

	/* should be verified on read */
	BUG_ON(get_unaligned_le16(&item->val_size) != get_unaligned_le16(&last->val_size));

	memcpy(val_ptr(item), val_ptr(last), get_unaligned_le16(&last->val_size));
}

void ngnfs_btree_init_block(struct ngnfs_btree_block *bt, u8 level)
{
	/* XXX do we want to zero the block here?  callers' responsibility? */
	bt->bnr = 0; /* XXX */
	bt->nr_items = 0;
	bt->total_free = cpu_to_le16(NGNFS_BTREE_MAX_FREE);
	bt->avail_free = bt->total_free;
	bt->level = level;
}

int ngnfs_btree_lookup(struct ngnfs_btree_block *bt, void *key, size_t key_size,
		       void *val, size_t val_size)
{
	struct btree_search_result res;
	struct ngnfs_btree_item *item;
	int ret;

	res = btree_search(bt, key, key_size);

	if (res.cmp == 0) {
		item = item_ptr(bt, res.pos);
		ret = min(val_size, get_unaligned_le16(&item->val_size));
		if (ret > 0)
			memcpy(val, val_ptr(item), ret);
	} else {
		ret = -ENOENT;
	}

	return ret;
}

int ngnfs_btree_insert(struct ngnfs_btree_block *bt, void *key, size_t key_size,
		       void *val, size_t val_size)
{
	struct btree_search_result res;
	int ret;

	if (WARN_ON_ONCE(key_size == 0 || key_size > NGNFS_BTREE_KEY_SIZE_MAX ||
			 val_size > NGNFS_BTREE_VAL_SIZE_MAX))
		return -EINVAL;

	res = btree_search(bt, key, key_size);

	if (res.cmp == 0) {
		ret = -EEXIST;
	} else {
		insert_item(bt, res.pos, key, key_size, val, val_size);
		ret = 0;
	}

	return ret;
}

int ngnfs_btree_delete(struct ngnfs_btree_block *bt, void *key, size_t key_size)
{
	struct btree_search_result res;
	int ret;

	res = btree_search(bt, key, key_size);

	if (res.cmp == 0) {
		remove_item(bt, res.pos);
		ret = 0;
	} else {
		ret = -ENOENT;
	}

	return ret;
}

/*
 * Split the items from a full block into its empty lesser sibling.
 * Moving items to the left maintains the separator key in the existing
 * block ref and we only have to add the new sibling parent item with
 * its new separator key.
 */
void ngnfs_btree_split(struct ngnfs_btree_block *parent, u16 bt_pos,
		       struct ngnfs_btree_block *bt, struct ngnfs_btree_block *sib)
{
	move_items(sib, bt, true, false);
	insert_parent_item(parent, bt_pos, sib);
}

/*
 * The destination btree block has fallen under the minimum number of
 * items.  Refill it from a neighbouring sibling, either balancing the
 * two or merging all of the sibling items into the block.
 *
 * If we emptied the sibling we remove the parent item referencing it.
 * If the sibling was greater (to the right) it's parent key can be a
 * separator between further blocks or the maximal key on the right
 * spline of the tree.  That can be different than the last key in the
 * block.  So if we remove a greater sibling we maintain its separator
 * key by having it reference the remaining block and remove the block's
 * parent item.
 *
 * The caller examines the sib block when it returns to find that it's
 * been emptied so it can free its bnr and drop it from the cache.
 */
void ngnfs_btree_refill(struct ngnfs_btree_block *parent, u16 bt_pos, u16 sib_pos,
			struct ngnfs_btree_block *bt, struct ngnfs_btree_block *sib)
{
	bool src_first = sib_pos > bt_pos;
	bool drain_src = used_pct(bt) + used_pct(sib) <= NGNFS_BTREE_MIN_USED_PCT * 2;

	move_items(bt, sib, src_first, drain_src);

	if (sib->nr_items != 0) {
		/* update the left parent's separator key between the two blocks */
		if (bt_pos < sib_pos)
			update_parent_key(parent, bt_pos, bt);
		else
			update_parent_key(parent, sib_pos, sib);
	} else {
		/* remove empty sib parent item, being careful to maintain greater separator */
		if (bt_pos < sib_pos) {
			update_parent_ref(parent, sib_pos, bt);
			remove_item(parent, bt_pos);
		} else {
			remove_item(parent, sib_pos);
		}
	}
}

static int cmp_item_off(const void *a, const void *b, const void *priv)
{
	const __le16 *off_a = a;
	const __le16 *off_b = b;

	return (int)le16_to_cpu(*off_b) - (int)le16_to_cpu(*off_a);
}

static int cmp_item_key(const void *a, const void *b, const void *priv)
{
	const __le16 *off_a = a;
	const __le16 *off_b = b;
	const struct ngnfs_btree_block *bt = priv;
	struct ngnfs_btree_item *item_a = (void *)bt + le16_to_cpu(*off_a);
	struct ngnfs_btree_item *item_b = (void *)bt + le16_to_cpu(*off_b);

	return cmp_keys(key_ptr(item_a), item_a->key_size,
			key_ptr(item_b), item_b->key_size);
}

/* kernel's lib/sort.c doesn't have a _16 variant */
static void swap_words_16(void *a, void *b, int n, const void *priv)
{
        do {
                u16 t = *(u16 *)(a + (n -= 4));
                *(u16 *)(a + n) = *(u16 *)(b + n);
                *(u16 *)(b + n) = t;
        } while (n);
}

/*
 * Move all the items to the end of the block so that all the free space
 * is gathered for allocation between the item offsets and the first
 * item.
 */
void ngnfs_btree_compact(struct ngnfs_btree_block *bt)
{
	struct ngnfs_btree_item *item;
	u16 size;
	u16 off;
	u16 nr;
	u16 i;

	if (bt->avail_free == bt->total_free)
		return;

	nr = le16_to_cpu(bt->nr_items);
	sort_r(&bt->item_off[0], nr, sizeof(bt->item_off[0]), cmp_item_off, swap_words_16, bt);

	off = NGNFS_BLOCK_SIZE;
	for (i = nr - 1; i >= 0; i--) {
		item = item_ptr(bt, i);
		size = item_size(item);
		off -= size;
		if (get_item_off(bt, i) != off) {
			set_item_off(bt, i, off);
			memmove(item_ptr(bt, i), item, size);
		}
	}

	bt->avail_free = bt->total_free;

	sort_r(&bt->item_off[0], nr, sizeof(bt->item_off[0]), cmp_item_key, swap_words_16, bt);
}

bool ngnfs_btree_verify(struct ngnfs_btree_block *bt)
{
	struct ngnfs_btree_item *item;
	struct ngnfs_btree_item *prev;
	unsigned long start;
	unsigned long size;
	unsigned long off;
	u16 free;
	u16 nr;
	u16 i;

	nr = le16_to_cpu(bt->nr_items);
	if (nr > NGNFS_BTREE_MAX_ITEMS)
		return false;

	sort_r(&bt->item_off[0], nr, sizeof(bt->item_off[0]), cmp_item_off, swap_words_16, bt);

	/* item payloads must be after the offset array and within the block, and not overlap */
	off = offsetof(struct ngnfs_btree_block, item_off[nr]);
	free = 0;
	for (i = 0; i < nr; i++) {
		start = get_item_off(bt, i);
		if (start < off || (start + sizeof(struct ngnfs_btree_item)) > NGNFS_BLOCK_SIZE)
			return false;

		item = item_ptr(bt, i);
		size = item_size(item);
		if (size > NGNFS_BLOCK_SIZE || start + size > NGNFS_BLOCK_SIZE)
			return false;

		if (start > off)
			free += start - off;

		off = start + size;
	}
	free += NGNFS_BLOCK_SIZE - off;

	/* avail_free can't overlap with any existing items */
	if (avail_free_end(bt) > get_item_off(bt, 0))
		return false;

	/* total free matches free space */
	if (le16_to_cpu(bt->total_free) != free)
		return false;

	/* XXX this will clobber(/fix) bad key sorting, but we can at least test dupes */
	sort_r(&bt->item_off[0], nr, sizeof(bt->item_off[0]), cmp_item_key, swap_words_16, bt);

	prev = item_ptr(bt, 0);
	for (i = 1; i < nr; i++) {
		item = item_ptr(bt, i);

		/* sorted keys must strictly increase */
		if (cmp_keys(key_ptr(item), item->key_size,
			     key_ptr(prev), prev->key_size) <= 0)
			return false;

		prev = item;
	}

	return true;
}
