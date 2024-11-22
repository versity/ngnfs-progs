/* SPDX-License-Identifier: GPL-2.0 */

/*
 * So far our use of work structs is minimal so we can get away with
 * using urcu's wait-free queue to queue work structs to a single
 * thread.
 */

#include "shared/lk/barrier.h"
#include "shared/lk/bitops.h"
#include "shared/lk/wait.h"
#include "shared/lk/workqueue.h"

#include "shared/thread.h"
#include "shared/urcu.h"

enum {
	WORK_QUEUED = 0,
};

/* never executed, indicates to the thread to stop */
static void WORK_DESTROY_FUNC(struct work_struct *work) { }

static void workqueue_thread(struct thread *thr, void *arg)
{
	struct workqueue_struct *wq = caa_container_of(thr, struct workqueue_struct, thr);
	struct cds_wfcq_node *node;
	struct work_struct *work;

	for (;;) {
		wait_event(&wq->waitq, !cds_wfcq_empty(&wq->head, &wq->tail) ||
			   thread_should_return(thr));

		node = __cds_wfcq_dequeue_nonblocking(&wq->head, &wq->tail);
		if (!node) /* XXX not possible today?  destroy queues _DESTROY_ func */
			break;

		/* testing the theory that a single dequeue will never need to block */
		assert(node != CDS_WFCQ_WOULDBLOCK);

		work = caa_container_of(node, struct work_struct, node);
		if (work->func == WORK_DESTROY_FUNC)
			break;

		/* this might not be needed, but we're being overly careful */
		cds_wfcq_node_init(&work->node);
		smp_wmb(); /* init node before clearing queued */

		clear_bit(WORK_QUEUED, &work->bits);
		/* func() can free, don't touch after */
		work->func(work);
		node = NULL;
		work = NULL;
	}
}

bool queue_work(struct workqueue_struct *wq, struct work_struct *work)
{
	bool newly_queued;

	newly_queued = !test_and_set_bit(WORK_QUEUED, &work->bits);
	if (newly_queued) {
		cds_wfcq_enqueue(&wq->head, &wq->tail, &work->node);
		wake_up(&wq->waitq);
	}

	return newly_queued;
}

struct workqueue_struct *create_singlethread_workqueue(char *name)
{
	struct workqueue_struct *wq;
	int ret;

	wq = malloc(sizeof(struct workqueue_struct));
	if (!wq)
		return NULL;

	thread_init(&wq->thr);
	init_waitqueue_head(&wq->waitq);
	cds_wfcq_init(&wq->head, &wq->tail);

	ret = thread_start(&wq->thr, workqueue_thread, NULL);
	if (ret < 0) {
		free(wq);
		return NULL;
	}

	return wq;
}

/*
 * This assumes that the caller has already stopped additional queueing.
 * This won't work for self-queueing work.
 */
void destroy_workqueue(struct workqueue_struct *wq)
{
	struct work_struct dummy_work;

	INIT_WORK(&dummy_work, WORK_DESTROY_FUNC);
	queue_work(wq, &dummy_work);
	thread_stop_wait(&wq->thr);

	assert(cds_wfcq_empty(&wq->head, &wq->tail));
	free(wq);
}
