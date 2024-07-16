/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_LK_SORT_H
#define NGNFS_SHARED_LK_SORT_H

typedef int (*cmp_r_func_t)(const void *a, const void *b, const void *priv);
typedef void (*swap_r_func_t)(void *a, void *b, int size, const void *priv);

void sort_r(void *base, size_t num, size_t size,
	    cmp_r_func_t cmp_func, swap_r_func_t swap_func, const void *priv);

#endif
