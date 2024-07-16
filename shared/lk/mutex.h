/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_LK_MUTEX_H
#define NGNFS_SHARED_LK_MUTEX_H

#include <pthread.h>

struct mutex {
	pthread_mutex_t ptm;
};

static inline void mutex_init(struct mutex *mutex)
{
	int ret;

	ret = pthread_mutex_init(&mutex->ptm, NULL);
	assert(ret == 0);
}

static inline void mutex_lock(struct mutex *mutex)
{
	int ret;

	ret = pthread_mutex_lock(&mutex->ptm);
	assert(ret == 0);
}

static inline void mutex_unlock(struct mutex *mutex)
{
	int ret;

	ret = pthread_mutex_unlock(&mutex->ptm);
	assert(ret == 0);
}

#endif
