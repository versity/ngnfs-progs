/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_LK_SLAB_H
#define NGNFS_SHARED_LK_SLAB_H

#include <stdlib.h>

typedef int gfp_t;

/*
 * only _ZERO does something.
 */
enum {
	GFP_KERNEL = (1 << 0),
	GFP_NOFS = (1 << 1),
	__GFP_ZERO = (1 << 2),
};

/*
 * It's weird, and we'll probably never use it, but it's easy enough to emulate.
 */
#define ZERO_SIZE_PTR ((void *)16)

static inline void *kzalloc(size_t sz, gfp_t flags)
{
	return sz ? calloc(1, sz) : ZERO_SIZE_PTR;
}

static inline void *kmalloc(size_t sz, gfp_t flags)
{
	if (flags & __GFP_ZERO)
		return kzalloc(sz, flags);
	else
		return sz ? malloc(sz) : ZERO_SIZE_PTR;
}

static inline void kfree(void *ptr)
{
	if (ptr > ZERO_SIZE_PTR)
		free(ptr);
}

#endif
