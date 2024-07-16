/* SPDX-License-Identifier: GPL-2.0 */

/*
 * We're using glibc's qsort_r which doesn't provide a custom swapping
 * func and doesn't make the priv arg const.  We have to be careful not
 * to do any weird magical things to swap.  We'll see how long we can
 * get away with this.
 */

#define _GNU_SOURCE
#include <stdlib.h>

#include "shared/lk/sort.h"

typedef int (*glibc_cmp_func_t)(const void *a, const void *b, void *priv);

void sort_r(void *base, size_t num, size_t size,
	    cmp_r_func_t cmp_func, swap_r_func_t swap_func, const void *priv)
{
	qsort_r(base, num, size, (glibc_cmp_func_t)cmp_func, (void *)priv);
}
