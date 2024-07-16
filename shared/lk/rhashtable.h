/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_LK_RHASHTABLE_H
#define NGNFS_SHARED_LK_RHASHTABLE_H

/*
 * Implement the subset of the rhashtable api that we use in terms of
 * the urcu lfht.
 */

#include "shared/lk/types.h"

#include "shared/urcu.h"

struct rhashtable_params {
	u16 key_len;
	u16 key_offset;
	u16 head_offset;
};

struct rhashtable {
	struct cds_lfht *lfht;
	struct rhashtable_params params;
};

struct rhash_head {
	struct cds_lfht_node node;
};

void *rhashtable_lookup(struct rhashtable *ht, const void *key,
			const struct rhashtable_params params);
void *rhashtable_lookup_get_insert_fast(struct rhashtable *ht, struct rhash_head *head,
					const struct rhashtable_params params);

int rhashtable_init(struct rhashtable *ht, const struct rhashtable_params *params);
void rhashtable_free_and_destroy(struct rhashtable *ht,
                                 void (*free_fn)(void *ptr, void *arg),
                                 void *arg);
void rhashtable_destroy(struct rhashtable *ht);

#endif
