/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_LK_WORKQUEUE_H
#define NGNFS_SHARED_LK_WORKQUEUE_H

#include "shared/thread.h"
#include "shared/urcu.h"

struct workqueue_struct {
	struct thread thr;
	wait_queue_head_t waitq;
	struct cds_wfcq_head head;
	struct cds_wfcq_tail tail;
};

struct work_struct;
typedef void (*work_func_t)(struct work_struct *work);

struct work_struct {
	struct cds_wfcq_node node;
        work_func_t func;
	unsigned long bits;
};

static inline void INIT_WORK(struct work_struct *work, work_func_t func)
{
	cds_wfcq_node_init(&work->node);
	work->func = func;
}

bool queue_work(struct workqueue_struct *wq, struct work_struct *work);

struct workqueue_struct *create_singlethread_workqueue(char *name);
void destroy_workqueue(struct workqueue_struct *wq);

#endif

