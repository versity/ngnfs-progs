/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Implement the subset of the rhashtable api that we use in terms of
 * the urcu lfht.
 */

#include "shared/lk/container_of.h"
#include "shared/lk/err.h"
#include "shared/lk/errno.h"
#include "shared/lk/jhash.h"
#include "shared/lk/rhashtable.h"
#include "shared/lk/string.h"

#include "shared/urcu.h"

static inline struct cds_lfht_node *head_to_node(struct rhash_head *head)
{
	return &head->node;
}

static inline struct rhash_head *node_to_head(struct cds_lfht_node *node)
{
	return container_of(node, struct rhash_head, node);
}

static inline void *head_to_obj(struct rhash_head *head, const struct rhashtable_params *params)
{
	return head ? ((void *)head - params->head_offset) : NULL;
}

static inline void *head_to_key(struct rhash_head *head, const struct rhashtable_params *params)
{
	return head ? (head_to_obj(head, params) + params->key_offset) : NULL;
}

static inline void *node_to_obj(struct cds_lfht_node *node, const struct rhashtable_params *params)
{
	return head_to_obj(node_to_head(node), params);
}

static inline void *node_to_key(struct cds_lfht_node *node, const struct rhashtable_params *params)
{
	return head_to_key(node_to_head(node), params);
}

/*
 * The lfht match function doesn't have a caller argument, so we wrap
 * all keys in a struct with the params that we can use to find the key
 * length and offset from the hashed node.  It also returns a boolean
 * value for whether the keys match, not a signed int comparison value.
 */
struct params_key {
	const struct rhashtable_params *params;
	const void *key;
};
static int match_node_key(struct cds_lfht_node *node, const void *key)
{
	const struct params_key *pk = key;

	return memcmp(node_to_key(node, pk->params), pk->key, pk->params->key_len) == 0;
}

/*
 * The caller holds the rcu_read_lock
 */
void *rhashtable_lookup(struct rhashtable *ht, const void *key,
			const struct rhashtable_params params)
{
	unsigned long hash = jhash(key, params.key_len, 0);
	struct params_key pk = { &params, key };
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;

	cds_lfht_lookup(ht->lfht, hash, match_node_key, &pk, &iter);
	node = cds_lfht_iter_get_node(&iter);

	return node_to_obj(node, &params);
}

/*
 * Returns NULL if the insertion was successful, existing object if one
 * was already present.  This is just a bit different than
 * _lfht_add_unique which returns the inserted node on success.
 *
 * The caller holds the rcu_read_lock.
 */
void *rhashtable_lookup_get_insert_fast(struct rhashtable *ht, struct rhash_head *head,
					const struct rhashtable_params params)
{
	struct cds_lfht_node *node = head_to_node(head);
	void *key = head_to_key(head, &params);
	unsigned long hash = jhash(key, params.key_len, 0);
	struct params_key pk = { &params, key };
	struct cds_lfht_node *existing;

	existing = cds_lfht_add_unique(ht->lfht, hash, match_node_key, &pk, node);
	if (existing != node)
		return node_to_obj(existing, &params);

	return NULL;
}

/*
 * XXX starting with simple fixed size for now.
 */
#define RHT_BUCKETS 1024
int rhashtable_init(struct rhashtable *ht, const struct rhashtable_params *params)
{
	ht->params = *params;

	ht->lfht = cds_lfht_new(RHT_BUCKETS, RHT_BUCKETS, RHT_BUCKETS, 0, NULL);
	if (!ht->lfht)
		return -ENOMEM;

	return 0;
}

void rhashtable_free_and_destroy(struct rhashtable *ht,
                                 void (*free_fn)(void *ptr, void *arg),
                                 void *arg)
{
	struct cds_lfht_iter iter;
	struct rhash_head *head;

	if  (!IS_ERR_OR_NULL(ht) && ht->lfht) {
		cds_lfht_for_each_entry(ht->lfht, &iter, head, node) {
			cds_lfht_del(ht->lfht, &head->node);
			if (free_fn)
				free_fn(head_to_obj(head, &ht->params), arg);
		}
		rhashtable_destroy(ht);
	}
}

void rhashtable_destroy(struct rhashtable *ht)
{
	if  (!IS_ERR_OR_NULL(ht) && ht->lfht) {
		cds_lfht_destroy(ht->lfht, NULL);
		ht->lfht = NULL;
	}
}
