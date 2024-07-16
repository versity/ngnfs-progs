/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_LK_KTIME_H
#define NGNFS_SHARED_LK_KTIME_H

#include <time.h>

#include "shared/lk/compiler.h"
#include "shared/lk/time64.h"
#include "shared/lk/types.h"

typedef s64     ktime_t;

static inline s64 ktime_to_ns(const ktime_t kt)
{
        return kt;
}

static inline ktime_t ktime_set(const s64 secs, const unsigned long nsecs)
{
        if (unlikely(secs >= KTIME_SEC_MAX))
                return KTIME_MAX;

        return (secs * NSEC_PER_SEC) + (s64)nsecs;
}

static inline ktime_t timespec_to_ktime(struct timespec ts)
{
        return ktime_set(ts.tv_sec, ts.tv_nsec);
}

extern ktime_t ktime_get_real(void);

#endif
