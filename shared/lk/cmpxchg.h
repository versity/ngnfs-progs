/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_LK_CMPXCHG_H
#define NGNFS_SHARED_LK_CMPXCHG_H

#include "shared/urcu.h"

#define cmpxchg uatomic_cmpxchg

#endif
