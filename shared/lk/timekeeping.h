/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_LK_TIMEKEEPING_H
#define NGNFS_SHARED_LK_TIMEKEEPING_H

#include "shared/lk/ktime.h"

ktime_t ktime_get_real(void);

static inline u64 ktime_get_real_ns(void)
{
        return ktime_to_ns(ktime_get_real());
}

#endif
