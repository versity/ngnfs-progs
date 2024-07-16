/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_LK_RCUPDATE_H
#define NGNFS_SHARED_LK_RCUPDATE_H

#include "shared/urcu.h"

/*
 * kfree_rcu() is tricky because it relies on the call_rcu machinery to
 * notice that an rcu_head offset is magic ( __is_kvfree_rcu_offset ).
 * urcu doesn't support this so we're stuck working with full rcu_head
 * and function pointers.  We magically assume that the rcu head pointer
 * == the pointer to free.  We're avoiding constant janitorial patches
 * to remove a call_rcu function that just frees.  I suppose we could
 * enforce this with build tooling.
 */
extern void urcu_kfree_head(struct rcu_head *head);
#define kfree_rcu(head) call_rcu(head, urcu_kfree_head)

/* we're not checking __rcu in userspace yet */
#define RCU_INITIALIZER(v) (typeof(*(v)) __force *)(v)
#define unrcu_pointer(p) ((typeof(*p) __force *)(p))

#endif
