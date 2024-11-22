/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_THREAD_H
#define NGNFS_SHARED_THREAD_H

#include <stdbool.h>
#include <pthread.h>

struct thread;
typedef void (*thread_fn_t)(struct thread *thr, void *arg);

struct thread {
	unsigned long bits;
	thread_fn_t fn;
	void *arg;
	pthread_t pthread;
};

int thread_prepare_main(void);
int thread_sigwait(void);
void thread_finish_main(void);

void thread_init(struct thread *thr);
int thread_start(struct thread *thr, thread_fn_t fn, void *arg);

bool thread_should_return(struct thread *thr);
void thread_stop_indicate(struct thread *thr);
void thread_stop_wait(struct thread *thr);
void thread_shutdown_all(void);

#endif
