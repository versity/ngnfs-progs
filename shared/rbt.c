/* SPDX-License-Identifier: GPL-2.0 */

/*
 * This implements the classic symmetrical rbtree with the array of
 * child pointers that you find in the literature.  Specifically, this
 * builds off the insertion and deletion case diagram in wikipedia's
 * page on rbtrees.
 *
 * The interface is modeled after the kernel's rbtree.h interface.  Our
 * core rbt code only deals with nodes.  The caller emebeds the nodes in
 * objects and is responsible for comparing keys and managing object
 * life cycles.
 *
 * This is used to index items in small blocks.  The node pointers are
 * not native memory pointers but are small fixed-endian byte offsets
 * from the root struct which is also embedded in the block.
 *
 * The bulk of the noise in the implementation, then, is going between
 * host pointers and fixed-endian byte offsets.  It's why so many
 * methods have a root argument that wouldn't otherwise.
 *
 * We also pass in the transaction block objects so that we can save
 * initial unmodified regions of the blocks before we store so that the
 * caller can undo changes made by the rbt.
 */

#include "shared/rbt.h"
#include "shared/txn.h"

#include "shared/lk/bug.h"

#define NGNFS_RBT_RED		(1U << 15)
#define NGNFS_RBT_BLACK		0
#define NGNFS_RBT_PARENT_MASK	(U16_MAX ^ NGNFS_RBT_RED)

void ngnfs_rbt_init_root(struct ngnfs_txn_block *tblk, struct ngnfs_rbt_root *root)
{
	ngnfs_tblk_assign(tblk, root->node, 0);
}

void ngnfs_rbt_init_node(struct ngnfs_txn_block *tblk, struct ngnfs_rbt_node *node)
{
	ngnfs_tblk_assign(tblk, node->_red_parent, 0);
	ngnfs_tblk_assign(tblk, node->child[NGNFS_RBT_LEFT], 0);
	ngnfs_tblk_assign(tblk, node->child[NGNFS_RBT_RIGHT], 0);
}

static inline void set_red_parent(struct ngnfs_txn_block *tblk, struct ngnfs_rbt_node *node,
				  u16 color, __le16 off)
{
	BUG_ON(color & NGNFS_RBT_PARENT_MASK);
	BUG_ON(off & cpu_to_le16(NGNFS_RBT_RED));

	ngnfs_tblk_assign(tblk, node->_red_parent,
			  cpu_to_le16(color) | (off & (cpu_to_le16(NGNFS_RBT_PARENT_MASK))));
}

/* flip the color without going through the full get/set color|off helpers */
static inline void flip_color(struct ngnfs_txn_block *tblk, struct ngnfs_rbt_node *node)
{
	ngnfs_tblk_assign(tblk, node->_red_parent, node->_red_parent ^ cpu_to_le16(NGNFS_RBT_RED));
}

static inline __le16 get_parent(struct ngnfs_rbt_node *node)
{
	return node->_red_parent & cpu_to_le16(NGNFS_RBT_PARENT_MASK);
}

static inline u16 get_color(struct ngnfs_rbt_node *node)
{
	return le16_to_cpu(node->_red_parent) & NGNFS_RBT_RED;
}

static inline void set_color(struct ngnfs_txn_block *tblk, struct ngnfs_rbt_node *node, u16 color)
{
	set_red_parent(tblk, node, color, get_parent(node));
}

static inline void set_parent(struct ngnfs_txn_block *tblk, struct ngnfs_rbt_node *node,
			      __le16 off)
{
	set_red_parent(tblk, node, get_color(node), off);
}

/*
 * null nodes are black.
 */
static inline bool is_red(struct ngnfs_rbt_node *node)
{
	return node && !!(le16_to_cpu(node->_red_parent) & NGNFS_RBT_RED);
}

static inline bool is_black(struct ngnfs_rbt_node *node)
{
	return !is_red(node);
}

/* return the direction of the only child */
static inline ngnfs_rbt_dir_t only_child_dir(struct ngnfs_rbt_node *node)
{
	return node->child[NGNFS_RBT_LEFT] ? NGNFS_RBT_LEFT : NGNFS_RBT_RIGHT;
}

static inline __le16 node_to_off(struct ngnfs_rbt_root *root, struct ngnfs_rbt_node *node)
{
	return node ? cpu_to_le16((void *)node - (void *)root) : 0;
}

static inline struct ngnfs_rbt_node *off_to_node(struct ngnfs_rbt_root *root, __le16 off)
{
	return off ? (void *)(root + le16_to_cpu(off)) : NULL;
}

/*
 * The node must currently be one of the children of the parent.
 */
static inline ngnfs_rbt_dir_t child_dir(struct ngnfs_rbt_root *root,
					struct ngnfs_rbt_node *parent, struct ngnfs_rbt_node *node)
{
	if (node_to_off(root, node) == parent->child[NGNFS_RBT_LEFT])
		return NGNFS_RBT_LEFT;
	else
		return NGNFS_RBT_RIGHT;
}

/*
 * Set the parent's link to the child, and the child's link to the parent.  If the parent is
 * null then we link from the root.  The child can be null as well.
 */
static void set_node_links(struct ngnfs_txn_block *tblk, struct ngnfs_rbt_root *root,
			   struct ngnfs_rbt_node *parent, ngnfs_rbt_dir_t dir,
			   struct ngnfs_rbt_node *node)
{
	__le16 off = node_to_off(root, node);

	if (parent)
		ngnfs_tblk_assign(tblk, parent->child[dir], off);
	else
		ngnfs_tblk_assign(tblk, root->node, off);

	if (node)
		set_parent(tblk, node, node_to_off(root, parent));
}

static void rotate(struct ngnfs_txn_block *tblk, struct ngnfs_rbt_root *root,
		   struct ngnfs_rbt_node *parent, ngnfs_rbt_dir_t dir)
{
	struct ngnfs_rbt_node *grand = off_to_node(root, get_parent(parent));
	struct ngnfs_rbt_node *node = off_to_node(root, parent->child[dir ^ 1]);
	struct ngnfs_rbt_node *child = off_to_node(root, node->child[dir]);

	set_node_links(tblk, root, grand, child_dir(root, grand, parent), node);
	set_node_links(tblk, root, node, dir, parent);
	set_node_links(tblk, root, parent, dir ^ 1, child);
}

/*
 * Swap the node in the tree with its in-order successor.  The node must
 * have both children so it will always have a successor, and the
 * successor by definition won't have a left child.
 *
 * This is a bit verbose but it makes it clear what's going on.  We
 * first dereference all the node pointers before updating any offsets.
 * Then we update all the links between the moving nodes and their
 * immediate linked neighbours.
 *
 * The confusing case to watch out for is when the successor is the
 * node's child.  We want to avoid naively creating loops by pointing
 * the node's right child (successor) at the successor, and by pointing
 * the successor's parent (node) to the node.
 */
static void swap_successor(struct ngnfs_txn_block *tblk, struct ngnfs_rbt_root *root,
			   struct ngnfs_rbt_node *node)
{
	struct ngnfs_rbt_node *parent = off_to_node(root, get_parent(node));
	struct ngnfs_rbt_node *left = off_to_node(root, node->child[NGNFS_RBT_LEFT]);
	struct ngnfs_rbt_node *right = off_to_node(root, node->child[NGNFS_RBT_RIGHT]);
	struct ngnfs_rbt_node *succ = ngnfs_rbt_successor(root, node);
	struct ngnfs_rbt_node *succ_parent = off_to_node(root, get_parent(succ));
	struct ngnfs_rbt_node *succ_right = off_to_node(root, succ->child[NGNFS_RBT_RIGHT]);

	set_node_links(tblk, root, parent, child_dir(root, parent, node), succ);
	set_node_links(tblk, root, succ, NGNFS_RBT_LEFT, left);
	if (succ_parent == node) {
		set_node_links(tblk, root, succ, NGNFS_RBT_RIGHT, node);
	} else {
		set_node_links(tblk, root, succ, NGNFS_RBT_RIGHT, right);
		set_node_links(tblk, root, succ_parent, child_dir(root, succ_parent, succ), node);
	}
	set_node_links(tblk, root, node, NGNFS_RBT_RIGHT, succ_right);
	ngnfs_tblk_assign(tblk, node->child[NGNFS_RBT_LEFT], 0);

	/* swap colors if they're different to maintain the traversal rb invariant */
	if (is_red(node) != is_red(succ)) {
		flip_color(tblk, node);
		flip_color(tblk, succ);
	}
}

static struct ngnfs_rbt_node *rbt_spine(struct ngnfs_rbt_root *root, ngnfs_rbt_dir_t dir)
{
	struct ngnfs_rbt_node *node = off_to_node(root, root->node);

	while (node && node->child[dir])
		node = off_to_node(root, node->child[dir]);

	return node;
}

struct ngnfs_rbt_node *ngnfs_rbt_first(struct ngnfs_rbt_root *root)
{
	return rbt_spine(root, NGNFS_RBT_LEFT);
}

struct ngnfs_rbt_node *ngnfs_rbt_last(struct ngnfs_rbt_root *root)
{
	return rbt_spine(root, NGNFS_RBT_RIGHT);
}

static struct ngnfs_rbt_node *rbt_advance(struct ngnfs_rbt_root *root, struct ngnfs_rbt_node *node,
					  ngnfs_rbt_dir_t dir)
{
	struct ngnfs_rbt_node *parent;

	/* descend down children in the advancing direction */
	if (node->child[dir]) {
		node = off_to_node(root, node->child[dir]);
		while (node->child[dir ^ 1])
			node = off_to_node(root, node->child[dir ^ 1]);
		return node;
	}

	/* no children, ascend through parents */
	while ((parent = off_to_node(root, get_parent(node))) &&
	       (child_dir(root, parent, node) == dir))
		node = parent;

	return parent;
}

struct ngnfs_rbt_node *ngnfs_rbt_prev(struct ngnfs_rbt_root *root, struct ngnfs_rbt_node *node)
{
	return rbt_advance(root, node, NGNFS_RBT_LEFT);
}

struct ngnfs_rbt_node *ngnfs_rbt_next(struct ngnfs_rbt_root *root, struct ngnfs_rbt_node *node)
{
	return rbt_advance(root, node, NGNFS_RBT_RIGHT);
}

static struct ngnfs_rbt_node *rbt_in_order(struct ngnfs_rbt_root *root,
					   struct ngnfs_rbt_node *node, ngnfs_rbt_dir_t dir)
{
        if (node && node->child[dir]) {
                node = off_to_node(root, node->child[dir]);
                while (node->child[dir ^ 1])
                        node = off_to_node(root, node->child[dir ^ 1]);
        }

        return node;
}

struct ngnfs_rbt_node *ngnfs_rbt_predecessor(struct ngnfs_rbt_root *root,
					     struct ngnfs_rbt_node *node)
{
	return rbt_in_order(root, node, NGNFS_RBT_LEFT);
}

struct ngnfs_rbt_node *ngnfs_rbt_successor(struct ngnfs_rbt_root *root,
					   struct ngnfs_rbt_node *node)
{
	return rbt_in_order(root, node, NGNFS_RBT_RIGHT);
}

static struct ngnfs_rbt_node *rbt_left_deepest(struct ngnfs_rbt_root *root,
					       struct ngnfs_rbt_node *node)
{
	while (node) {
		if (node->child[NGNFS_RBT_LEFT])
			node = off_to_node(root, node->child[NGNFS_RBT_LEFT]);
		else if (node->child[NGNFS_RBT_RIGHT])
			node = off_to_node(root, node->child[NGNFS_RBT_RIGHT]);
		else
			break;
	}

	return node;
}

struct ngnfs_rbt_node *ngnfs_rbt_first_postorder(struct ngnfs_rbt_root *root)
{
	return rbt_left_deepest(root, off_to_node(root, root->node));
}

struct ngnfs_rbt_node *ngnfs_rbt_next_postorder(struct ngnfs_rbt_root *root,
						struct ngnfs_rbt_node *node)
{
	struct ngnfs_rbt_node *parent;

	if (!node)
		return NULL;

	parent = off_to_node(root, get_parent(node));
	if (parent && child_dir(root, parent, node) == NGNFS_RBT_LEFT &&
	    parent->child[NGNFS_RBT_RIGHT])
                return rbt_left_deepest(root, off_to_node(root, parent->child[NGNFS_RBT_RIGHT]));
	else
		return parent;
}

void ngnfs_rbt_insert(struct ngnfs_txn_block *tblk, struct ngnfs_rbt_root *root,
		      struct ngnfs_rbt_node *parent, ngnfs_rbt_dir_t ins_dir,
		      struct ngnfs_rbt_node *node)
{
	struct ngnfs_rbt_node *grand;
	struct ngnfs_rbt_node *uncle;
	ngnfs_rbt_dir_t dir;

	ngnfs_rbt_init_node(tblk, node);
	set_color(tblk, node, NGNFS_RBT_RED);

	set_node_links(tblk, root, parent, ins_dir, node);

	if (!parent)
		return;

	do {
		/* I1 */
		if (is_black(parent))
			return;

		grand = off_to_node(root, get_parent(parent));
		if (!grand) {
			/* I4 */
			set_color(tblk, parent, NGNFS_RBT_BLACK);
			return;
		}

		dir = child_dir(root, grand, parent);
		uncle = off_to_node(root, grand->child[dir ^ 1]);
		if (is_black(uncle)) {
			/* I56 */
			if (node == off_to_node(root, parent->child[dir ^ 1])) {
				/* I5 */
				rotate(tblk, root, parent, dir);
				node = parent;
				parent = off_to_node(root, grand->child[dir]);
			}
			/* I6 */
			rotate(tblk, root, grand, dir ^ 1);
			set_color(tblk, parent, NGNFS_RBT_BLACK);
			set_color(tblk, grand, NGNFS_RBT_RED);
			return;
		}

		/* I2 */
		set_color(tblk, parent, NGNFS_RBT_BLACK);
		set_color(tblk, uncle, NGNFS_RBT_BLACK);
		set_color(tblk, grand, NGNFS_RBT_RED);
		node = grand;
	} while ((parent = off_to_node(root, get_parent(node))) != NULL);

	/* I3 */
}

void ngnfs_rbt_delete(struct ngnfs_txn_block *tblk, struct ngnfs_rbt_root *root,
		      struct ngnfs_rbt_node *node)
{
	struct ngnfs_rbt_node *parent;
	struct ngnfs_rbt_node *child;
	struct ngnfs_rbt_node *sibling;
	struct ngnfs_rbt_node *distant;
	struct ngnfs_rbt_node *close;
	ngnfs_rbt_dir_t dir;
	bool was_red;

	/* make sure node doesn't have two children */
	if (node->child[NGNFS_RBT_LEFT] && node->child[NGNFS_RBT_RIGHT])
		swap_successor(tblk, root, node);

	/* link parent (or root) to child (maybe null) */
	parent = off_to_node(root, get_parent(node));
	dir = child_dir(root, parent, node);
	child = off_to_node(root, node->child[NGNFS_RBT_LEFT] ?: node->child[NGNFS_RBT_RIGHT]);
	set_node_links(tblk, root, parent, child_dir(root, parent, node), child);

	/* wipe node before all the return cases */
	was_red = is_red(node);
	ngnfs_rbt_init_node(tblk, node);

	/* single child must have been read, deleted must have been black */
	if (child) {
		set_color(tblk, child, NGNFS_RBT_BLACK);
		return;
	}

	/* done when it was only node (no children or parent) or was red */
	if (!parent || was_red)
		return;

	/* in first iteration node is unlinked, can't check parent dir */
	goto unlinked_node;

	do {
		dir = child_dir(root, parent, node);
unlinked_node:
		sibling = off_to_node(root, parent->child[dir ^ 1]);
		distant = off_to_node(root, sibling->child[dir ^ 1]);
		close = off_to_node(root, sibling->child[dir]);
		if (is_red(sibling))
			goto D3;
		if (is_red(distant))
			goto D6;
		if (is_red(close))
			goto D5;
		if (is_red(parent))
			goto D4;

		/* D2 */
		set_color(tblk, sibling, NGNFS_RBT_RED);
		node = parent;
	} while ((parent = off_to_node(root, get_parent(node))) != NULL);

	/* D1 */
	return;

D3:
	rotate(tblk, root, parent, dir);
	set_color(tblk, parent, NGNFS_RBT_RED);
	set_color(tblk, sibling, NGNFS_RBT_BLACK);
	sibling = close;
	distant = off_to_node(root, sibling->child[dir ^ 1]);
	if (is_red(distant))
		goto D6;
	close = off_to_node(root, sibling->child[dir]);
	if (is_red(close))
		goto D5;

D4:
	set_color(tblk, sibling, NGNFS_RBT_RED);
	set_color(tblk, parent, NGNFS_RBT_BLACK);
	return;

D5:
	rotate(tblk, root, sibling, dir ^ 1);
	set_color(tblk, sibling, NGNFS_RBT_RED);
	set_color(tblk, close, NGNFS_RBT_BLACK);
	distant = sibling;
	sibling = close;

D6:
	rotate(tblk, root, parent, dir);
	set_color(tblk, sibling, get_color(parent));
	set_color(tblk, parent, NGNFS_RBT_BLACK);
	set_color(tblk, distant, NGNFS_RBT_BLACK);
}

/*
 * The caller has already moved a node from an old memory location to a
 * new one.  The old and new locations may overlap so the old address
 * can not be dereferenced.  We follow the node's outgoing links to
 * update each neighbor's incoming link.
 *
 * This is a relatively hot path in btree node compaction so we're
 * trying to keep it light while still not too obtuse.  We could reuse
 * set_node_links but it would store the node's outgoing links and
 * they're still correct.
 */
void ngnfs_rbt_moved(struct ngnfs_txn_block *tblk, struct ngnfs_rbt_root *root,
		     struct ngnfs_rbt_node *node, void *old)
{
	__le16 node_off = node_to_off(root, node);
	__le16 old_off = node_to_off(root, old);
	struct ngnfs_rbt_node *nei;

	if ((nei = off_to_node(root, get_parent(node)))) {
		if (nei->child[NGNFS_RBT_LEFT] == old_off)
			ngnfs_tblk_assign(tblk, nei->child[NGNFS_RBT_LEFT], node_off);
		else
			ngnfs_tblk_assign(tblk, nei->child[NGNFS_RBT_RIGHT], node_off);
	} else {
		ngnfs_tblk_assign(tblk, root->node, node_off);
	}

	if ((nei = off_to_node(root, node->child[NGNFS_RBT_LEFT])))
		set_parent(tblk, nei, node_to_off(root, node));
	if ((nei = off_to_node(root, node->child[NGNFS_RBT_RIGHT])))
		set_parent(tblk, nei, node_to_off(root, node));
}
