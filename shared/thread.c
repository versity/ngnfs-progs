/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Light wrappers around pthreads that take care of the particulars of
 * our signal handling and urcu thread registration.
 */

#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <urcu.h>
#include <assert.h>

#include "shared/lk/bitops.h"

#include "shared/log.h"
#include "shared/thread.h"
#include "shared/trace.h"

enum {
	THR_CREATED = 0,
	THR_SHOULD_RETURN,
};

static void register_thread(void)
{
	int ret;

	rcu_register_thread();
	ret = trace_register_thread();
	assert(ret == 0); /* XXX */
}

static void unregister_thread(void)
{
	trace_unregister_thread();
	rcu_unregister_thread();
}

/*
 * Typically our thread wrappers have a chance to register all threads
 * before they execute.  main() is obviously the exception and this lets
 * them call the same thread registration code as the other threads.
 *
 * We also take the opportunity to block all signals that all threads
 * will inherit, so that explicit sigwait can be used to control where
 * signals arrive.
 */
int thread_prepare_main(void)
{
	sigset_t set;
	int ret;

	sigfillset(&set);

	ret = pthread_sigmask(SIG_SETMASK, &set, NULL);
	if (ret != 0) {
		ret = -errno;
		log("error masking signals: "ENOF, ENOA(-ret));
	}

	ret = trace_init();
	if (ret == 0)
		register_thread();

	return ret;
}

/*
 * This must be called after _prepare_main succeeds and should be called
 * after all other layers has been shut down.
 */
void thread_finish_main(void)
{
	unregister_thread();
	trace_destroy();
}

/*
 * Having blocked signals for other threads, block waiting for signals
 * in a main monitoring thread so other threads aren't affected.
 */
int thread_sigwait(void)
{
	sigset_t set;
	int sig;
	int ret;

	for (;;) {
		sigfillset(&set);
		ret = sigwait(&set, &sig);
		if (ret != 0) {
			ret = -errno;
			log("error waiting for signal: "ENOF, ENOA(-ret));
			break;
		}

		printf("got signal %u, exiting\n", sig);
		trace_flush();
		exit(1);
	}

	return ret;
}

static void *thread_fn_wrapper(void *arg)
{
	struct thread *thr = arg;

	register_thread();

	thr->fn(thr, thr->arg);

	unregister_thread();

	return NULL;
}

void thread_init(struct thread *thr)
{
	thr->bits = 0;
}

int thread_start(struct thread *thr, thread_fn_t fn, void *arg)
{
	int ret;

	thr->fn = fn;
	thr->arg = arg;

	ret = pthread_create(&thr->pthread, NULL, thread_fn_wrapper, thr);
	if (ret != 0) {
		ret = -ret;
	} else {
		set_bit(THR_CREATED, &thr->bits);
		ret = 0;
	}

	return ret;
}

bool thread_should_return(struct thread *thr)
{
	return test_bit(THR_SHOULD_RETURN, &thr->bits);
}

void thread_stop_indicate(struct thread *thr)
{
	set_bit(THR_SHOULD_RETURN, &thr->bits);
}

void thread_stop_wait(struct thread *thr)
{
	int ret;

	if (test_bit(THR_CREATED, &thr->bits)) {
		thread_stop_indicate(thr);
		ret = pthread_join(thr->pthread, NULL);
		assert(ret == 0);
	}
}
