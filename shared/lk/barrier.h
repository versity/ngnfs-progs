/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_LK_BARRIER_H
#define NGNFS_SHARED_LK_BARRIER_H

#include "shared/urcu.h"

#define smp_mb		cmm_smp_mb
#define smp_rmb		cmm_smp_rmb
#define smp_wmb		cmm_smp_wmb

#endif
