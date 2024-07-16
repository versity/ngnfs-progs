/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_LK_BUILD_BUG_H
#define NGNFS_SHARED_LK_BUILD_BUG_H

#include <assert.h>

#define BUILD_BUG_ON(cond) \
        static_assert(!(cond), "!(" #cond ")")

#endif
