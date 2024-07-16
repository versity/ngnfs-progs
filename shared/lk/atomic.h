/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_LK_ATOMIC_H
#define NGNFS_SHARED_LK_ATOMIC_H

#include <stdbool.h>

#include "shared/lk/types.h"

#include "shared/urcu.h"

#define gen_atomics_full(ATOMIC, PREFIX, TYPE)				\
typedef struct {							\
	/*								\
	 * XXX annoyingly, older urcu atomic versions implement some	\
	 * pointer casting in terms of a fake array of longs.  It	\
	 * generates fine code, but gcc can get angry that dereferencing \
	 * the fake struct far exceeds the allocation of the atomic	\
	 * type's size.  We blow out the size of atomics in memory to	\
	 * avoid the warning.  We could detect this at build time, or	\
	 * just require more recent library versions.			\
	 */								\
	union {								\
		long _dummy[10];					\
		TYPE counter;						\
	};								\
} ATOMIC;								\
									\
static inline void PREFIX##set(ATOMIC *v, TYPE i)			\
{									\
	v->counter = i;							\
}									\
									\
static inline TYPE PREFIX##read(ATOMIC *v)				\
{									\
	return uatomic_read(&v->counter);				\
}									\
									\
static inline void PREFIX##inc(ATOMIC *v)				\
{									\
	uatomic_inc(&v->counter);					\
}									\
									\
static inline void PREFIX##dec(ATOMIC *v)				\
{									\
	uatomic_dec(&v->counter);					\
}									\
									\
static inline TYPE PREFIX##inc_return(ATOMIC *v)			\
{									\
	return uatomic_add_return(&v->counter, 1);			\
}									\
									\
static inline TYPE PREFIX##dec_return(ATOMIC *v)			\
{									\
	return uatomic_add_return(&v->counter, -1);			\
}									\
									\
static inline bool PREFIX##dec_and_test(ATOMIC *v)			\
{									\
	return uatomic_add_return(&v->counter, -1) == 0;		\
}									\
									\
static inline void PREFIX##add(TYPE i, ATOMIC *v)			\
{									\
	uatomic_add(&v->counter, i);					\
}									\
									\
static inline void PREFIX##sub(TYPE i, ATOMIC *v)			\
{									\
	uatomic_sub(&v->counter, i);					\
}									\
									\
static inline TYPE PREFIX##cmpxchg(ATOMIC *v, TYPE old, TYPE new)	\
{									\
	return uatomic_cmpxchg(&v->counter, old, new);			\
}

#define gen_atomics(SEP, TYPE) \
	gen_atomics_full(atomic##SEP##t, atomic##SEP, TYPE)

/*
 * The atomic function prefixes have varied naming conventions:
 *   int atomic_read(..
 *   long atomic_long_read(..
 *   s64 atomic64_read(..
 */
gen_atomics(_, int)
gen_atomics(_long_, long)
gen_atomics(64_, s64)

#endif
