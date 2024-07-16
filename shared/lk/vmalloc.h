/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_LK_VMALLOC_H
#define NGNFS_SHARED_LK_VMALLOC_H

static inline void *vmalloc(unsigned long size)
{
	return malloc(size);
}

static inline void vfree(void *addr)
{
	return free(addr);
}

#endif
