/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_LK_STDDEF_H
#define NGNFS_SHARED_LK_STDDEF_H

#include <unistd.h>
#include <stddef.h>
#include <stdbool.h>

#undef NULL
#define NULL ((void *)0)

#undef offsetof
#define offsetof(TYPE, MEMBER)  __builtin_offsetof(TYPE, MEMBER)

/**
 * sizeof_field() - Report the size of a struct field in bytes
 *
 * @TYPE: The structure containing the field of interest
 * @MEMBER: The field to return the size of
 */
#define sizeof_field(TYPE, MEMBER) sizeof((((TYPE *)0)->MEMBER))

/**
 * offsetofend() - Report the offset of a struct field within the struct
 *
 * @TYPE: The type of the structure
 * @MEMBER: The member within the structure to get the end offset of
 */
#define offsetofend(TYPE, MEMBER) \
        (offsetof(TYPE, MEMBER) + sizeof_field(TYPE, MEMBER))

#endif
