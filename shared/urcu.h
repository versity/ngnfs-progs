/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_URCU_H
#define NGNFS_SHARED_URCU_H

/*
 * Provide a consistent urcu flavor for all users.
 */
#define URCU_API_MAP
#include <urcu/urcu-memb.h>

#include <urcu/uatomic.h>
#include <urcu/compiler.h>
#include <urcu/rculist.h>
#include <urcu/list.h>
#include <urcu/wfstack.h>
#include <urcu/rculfhash.h>
#include <urcu/tls-compat.h>

#endif

