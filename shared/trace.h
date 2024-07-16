/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_TRACE_H
#define NGNFS_SHARED_TRACE_H

#include <stdio.h>

#include "shared/lk/types.h"

#include "shared/urcu.h"

/*
 * These are used by the generated trace event recording functions.
 */
#define trace_store_begin()	\
do {				\
	rcu_read_lock();	\
} while (0)

#define trace_store_end()	\
do {				\
	rcu_read_unlock();	\
} while (0)

void *trace_store_ptr(u16 id, size_t len);
void trace_flush(void);

int trace_register_thread(void);
void trace_unregister_thread(void);

int trace_init(void);
int trace_setup(char *trace_path);
void trace_destroy(void);

#include "generated-trace-inlines.h"

#endif
