/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_LK_TIME64_H
#define NGNFS_SHARED_LK_TIME64_H

/* from include/vdso/time64.h */

#define MSEC_PER_SEC    1000L
#define USEC_PER_MSEC   1000L
#define NSEC_PER_USEC   1000L
#define NSEC_PER_MSEC   1000000L
#define USEC_PER_SEC    1000000L
#define NSEC_PER_SEC    1000000000L
#define FSEC_PER_SEC    1000000000000000LL

#define KTIME_MAX                       ((s64)~((u64)1 << 63))
#define KTIME_MIN                       (-KTIME_MAX - 1)
#define KTIME_SEC_MAX                   (KTIME_MAX / NSEC_PER_SEC)
#define KTIME_SEC_MIN                   (KTIME_MIN / NSEC_PER_SEC)

#endif
