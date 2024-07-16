/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_LK_MATH64_H
#define NGNFS_SHARED_LK_MATH64_H

#include "shared/lk/types.h"

/*
 * We don't mind expensive 64bit division on 32bit in userspace.
 */
static inline u64 div_u64_rem(u64 dividend, u32 divisor, u32 *remainder)
{
        *remainder = dividend % divisor;
        return dividend / divisor;
}

#endif
