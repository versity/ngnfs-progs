/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_LK_COMPILER_ATTRIBUTES_H
#define NGNFS_SHARED_LK_COMPILER_ATTRIBUTES_H

#ifndef __always_inline
#define __always_inline                 inline __attribute__((__always_inline__))
#endif

#define __must_check                    __attribute__((__warn_unused_result__))
#define __no_kasan_or_inline		__always_inline
#define __no_sanitize_or_inline		__no_kasan_or_inline
#define __packed                        __attribute__((__packed__))
#define __printf(a, b)			__attribute__((__format__(printf, a, b)))

/*
 * <linux/types.h> defines __bitwise
 */
#ifdef __CHECKER__
# define __force	__attribute__((force))
#else
# define __force
#endif

#endif
