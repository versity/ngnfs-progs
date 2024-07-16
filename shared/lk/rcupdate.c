/* SPDX-License-Identifier: GPL-2.0 */

#include "shared/lk/rcupdate.h"

/* see kfree_rcu() */
void urcu_kfree_head(struct rcu_head *head)
{
	free(head);
}
