/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_LK_CONTAINER_OF_H
#define NGNFS_SHARED_LK_CONTAINER_OF_H

#include "shared/lk/stddef.h"

#define typeof_member(T, m)     typeof(((T*)0)->m)

/**
 * container_of - cast a member of a structure out to the containing structure
 * @ptr:        the pointer to the member.
 * @type:       the type of the container struct this is embedded in.
 * @member:     the name of the member within the struct.
 *
 * WARNING: any const qualifier of @ptr is lost.
 */
#define container_of(ptr, type, member) ({                              \
        void *__mptr = (void *)(ptr);                                   \
        ((type *)(__mptr - offsetof(type, member))); })

#endif

