/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_BTREE_H
#define NGNFS_SHARED_BTREE_H

#include "shared/format-block.h"

void ngnfs_btree_init_block(struct ngnfs_btree_block *bt, u8 level);

int ngnfs_btree_lookup(struct ngnfs_btree_block *bt, void *key, size_t key_size,
		       void *val, size_t val_size);
int ngnfs_btree_insert(struct ngnfs_btree_block *bt, void *key, size_t key_size,
		       void *val, size_t val_size);
int ngnfs_btree_delete(struct ngnfs_btree_block *bt, void *key, size_t key_size);

void ngnfs_btree_split(struct ngnfs_btree_block *parent, u16 bt_pos,
		       struct ngnfs_btree_block *bt, struct ngnfs_btree_block *sib);
void ngnfs_btree_refill(struct ngnfs_btree_block *parent, u16 bt_pos, u16 sib_pos,
			struct ngnfs_btree_block *bt, struct ngnfs_btree_block *sib);
void ngnfs_btree_compact(struct ngnfs_btree_block *bt);
bool ngnfs_btree_verify(struct ngnfs_btree_block *bt);

#endif
