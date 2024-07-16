/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_LK_COMPILER_H
#define NGNFS_SHARED_LK_COMPILER_H

#define likely(x)       __builtin_expect(!!(x), 1)
#define unlikely(x)     __builtin_expect(!!(x), 0)

#endif

