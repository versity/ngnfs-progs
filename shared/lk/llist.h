/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_LK_LLIST_H
#define NGNFS_SHARED_LK_LLIST_H

/*
 * Implementing the kernel's llist interface for userspace in terms of
 * rcu's wfstack, which itself is a port of the kernel's llist
 * implementation.
 */

#include "shared/urcu.h"

/*
 * We don't use the variant with the lock, we only support the op
 * combinations (many add(push), one del_all(pop_all)) that don't
 * require synchronization.
 */
struct llist_head {
	struct __cds_wfs_stack wfstack;
};

struct llist_node {
	struct cds_wfs_node wfnode;
};

#define LLIST_HEAD_INIT(name)   { NULL }
#define LLIST_HEAD(name)        struct llist_head name = LLIST_HEAD_INIT(name)

static inline void init_llist_head(struct llist_head *list)
{
	__cds_wfs_init(&list->wfstack);
}

/*
 * XXX this doesn't exist upstream.  llist_add() doesn't have an
 * assertion that the node was initialized, but cds_wfs_push does.
 */
static inline void init_llist_node(struct llist_node *node)
{
	cds_wfs_node_init(&node->wfnode);
}

static inline bool llist_empty(struct llist_head *head)
{
        return cds_wfs_empty(&head->wfstack);
}

/*
 * llist_add: " * Returns true if the list was empty prior to adding this entry."
 * cds_wfs_push: " * Returns 0 if the stack was empty prior to adding the node."
 */
static inline bool llist_add(struct llist_node *new, struct llist_head *head)
{
	return !cds_wfs_push(&head->wfstack, &new->wfnode);
}

static inline struct llist_node *llist_del_all(struct llist_head *head)
{
	struct cds_wfs_head *wfhead;
	struct cds_wfs_node *wfnode;

	wfhead = __cds_wfs_pop_all(&head->wfstack);
	wfnode = cds_wfs_first(wfhead);
	if (wfnode)
		return caa_container_of(wfnode, struct llist_node, wfnode);
	else
		return NULL;
}

/*
 * The wfstack introduces heads for starting iteration which are wrapped
 * nodes with magic terminal values that we have to test with functions
 * that return null.
 */
#define _llist_for_each_entry_first(pos, llnode, member)					\
({												\
	__typeof__(pos) _pos;									\
	__typeof__(llnode) _llnode = (llnode);							\
	struct cds_wfs_head *_wfhead;								\
	struct cds_wfs_node *_wfnode;								\
												\
	if (_llnode) {										\
		_wfhead = caa_container_of(&(llnode)->wfnode, struct cds_wfs_head, node);	\
		_wfnode = cds_wfs_first(_wfhead);						\
	} else {										\
		_wfnode = NULL;									\
	}											\
												\
	if (_wfnode)										\
		_pos = caa_container_of(_wfnode, __typeof__(*_pos), member.wfnode);		\
	else											\
		_pos = NULL;									\
												\
	_pos;											\
})

#define _llist_for_each_entry_next(pos, member)							\
({												\
	__typeof__(pos) _pos = (pos);								\
	struct cds_wfs_node *_wfnode;								\
												\
	_wfnode = cds_wfs_next_nonblocking(&(pos)->member.wfnode);				\
	if (_wfnode)										\
		_pos = caa_container_of(_wfnode, __typeof__(*_pos), member.wfnode);		\
	else											\
		_pos = NULL;									\
												\
	_pos;											\
})

#define llist_for_each_entry(pos, node, member)							\
        for (pos = _llist_for_each_first(pos, node, member);					\
	     pos != NULL;									\
             pos = _llist_for_each_next(pos, member))						\

#define _llist_for_each_first(pos, llnode)							\
({												\
	__typeof__(llnode) _llnode = (llnode);							\
	struct cds_wfs_head *_wfhead;								\
	struct cds_wfs_node *_wfnode;								\
	__typeof__(pos) _pos;									\
												\
	if (_llnode) {										\
		_wfhead = caa_container_of(&(llnode)->wfnode, struct cds_wfs_head, node);	\
		_wfnode = cds_wfs_first(_wfhead);						\
	} else {										\
		_wfnode = NULL;									\
	}											\
												\
	if (_wfnode)										\
		_pos = caa_container_of(_wfnode, struct llist_node, wfnode);			\
	else											\
		_pos = NULL;									\
												\
	_pos;											\
})

#define _llist_for_each_next(pos)								\
({												\
	struct cds_wfs_node *_wfnode;								\
	__typeof__(pos) _pos;									\
												\
	_wfnode = cds_wfs_next_nonblocking(&(pos)->wfnode);					\
	if (_wfnode)										\
		_pos = caa_container_of(_wfnode, struct llist_node, wfnode);			\
	else											\
		_pos = NULL;									\
												\
	_pos;											\
})

#define llist_for_each(pos, node)								\
        for (pos = _llist_for_each_first(pos, node);						\
	     pos != NULL;									\
             pos = _llist_for_each_next(pos))							\

#endif
