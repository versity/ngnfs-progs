/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_LK_WAIT_H
#define NGNFS_SHARED_LK_WAIT_H

/*
 * So far we've only needed the basic
 * wait_event/waitqueue_active/wake_up pattern so we can implement it
 * with futexes and atomics.
 *
 * It's not portable, but it's easy and quick.  We could go for more
 * portable and heavy implementations in terms of pthread mutexes and
 * conds or urcu blocking data structures.
 */

#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <assert.h>

#include "shared/urcu.h"

struct wait_queue_head {
	unsigned long nr_waiting;
	uint32_t wake_counter;
};
typedef struct wait_queue_head wait_queue_head_t;

static inline void init_waitqueue_head(wait_queue_head_t *wq)
{
	uatomic_set(&wq->nr_waiting, 0);
	wq->wake_counter = 0;
}

/*
 * Waking updates the condition then tests if the waitq is active and
 * wakes.  We want to set waiting and test the condition in the opposite
 * order to avoid missed wakeups and deadlocks. The outer condition test
 * can only avoid sleeping which can't cause a deadlock.
 *
 * We use a u32 as the wake up counter to ensure that a wake up hits a
 * waiter who has tested the condition before they start to wait.
 * There's the possibility that it wraps.
 */
#define wait_event(wq_head, condition)								\
do {												\
	__typeof__(wq_head) _wq = (wq_head);							\
	uint32_t _ctr;										\
	long _ret;										\
												\
        if (!(condition)) {									\
		uatomic_inc(&_wq->nr_waiting);							\
		for (;;) {									\
			_ctr = uatomic_read(&_wq->wake_counter);				\
			cmm_barrier();								\
			if (condition)								\
				break;								\
			_ret = syscall(SYS_futex, &_wq->wake_counter, FUTEX_WAIT,_ctr,		\
				      NULL, NULL, 0);						\
			assert(_ret == 0 ||							\
			       (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR));	\
		}										\
		uatomic_dec(&_wq->nr_waiting);							\
	}											\
} while (0)

/*
 * The caller is responsible for ordering of sleeping and waking.  This
 * implementation just needs to make sure that concurrent sleeping and
 * waking can't race to miss the wake up.  We always increment the wake
 * counter and only call futex wake if we see waiters.
 */
static inline void wake_up(wait_queue_head_t *wq)
{
	long ret;

	uatomic_inc(&wq->wake_counter);
	cmm_barrier();
	if (uatomic_read(&wq->nr_waiting) > 0) {
		ret = syscall(SYS_futex, &wq->wake_counter, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
		assert(ret >= 0);
	}
}

static inline int waitqueue_active(struct wait_queue_head *wq)
{
	return !!uatomic_read(&wq->nr_waiting);
}

#endif
