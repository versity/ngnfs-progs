/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_RBT_H
#define NGNFS_SHARED_RBT_H

#include "shared/format-block.h"
#include "shared/txn.h"

void ngnfs_rbt_init_root(struct ngnfs_txn_block *tblk, struct ngnfs_rbt_root *root);
void ngnfs_rbt_init_node(struct ngnfs_txn_block *tblk, struct ngnfs_rbt_node *node);

struct ngnfs_rbt_node *ngnfs_rbt_first(struct ngnfs_rbt_root *root);
struct ngnfs_rbt_node *ngnfs_rbt_last(struct ngnfs_rbt_root *root);
struct ngnfs_rbt_node *ngnfs_rbt_prev(struct ngnfs_rbt_root *root, struct ngnfs_rbt_node *node);
struct ngnfs_rbt_node *ngnfs_rbt_next(struct ngnfs_rbt_root *root, struct ngnfs_rbt_node *node);
struct ngnfs_rbt_node *ngnfs_rbt_predecessor(struct ngnfs_rbt_root *root,
					     struct ngnfs_rbt_node *node);
struct ngnfs_rbt_node *ngnfs_rbt_successor(struct ngnfs_rbt_root *root,
					   struct ngnfs_rbt_node *node);
struct ngnfs_rbt_node *ngnfs_rbt_first_postorder(struct ngnfs_rbt_root *root);
struct ngnfs_rbt_node *ngnfs_rbt_next_postorder(struct ngnfs_rbt_root *root,
						struct ngnfs_rbt_node *node);

void ngnfs_rbt_insert(struct ngnfs_txn_block *tblk, struct ngnfs_rbt_root *root,
		      struct ngnfs_rbt_node *parent, ngnfs_rbt_dir_t ins_dir,
		      struct ngnfs_rbt_node *node);
void ngnfs_rbt_delete(struct ngnfs_txn_block *tblk, struct ngnfs_rbt_root *root,
			struct ngnfs_rbt_node *node);
void ngnfs_rbt_moved(struct ngnfs_txn_block *tblk, struct ngnfs_rbt_root *root,
		     struct ngnfs_rbt_node *node, void *old);

#endif
